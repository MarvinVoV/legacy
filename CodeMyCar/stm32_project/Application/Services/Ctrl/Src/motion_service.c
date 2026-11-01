/*
 * motion_service.c
 *
 *  Created on: Apr 6, 2025
 *      Author: marvin
 */
#include "motion_service.h"
#include "log_manager.h"
#include "error_code.h"
#include <math.h>
#include "motion_task.h"
#include "motion_kinematics.h"
#include "safety_service.h"
#include "sonar_task.h"
#include "ultrasonic_config.h"
#include "steering_map.h"


//------------------------------------------------------------------------------
// 常量定义
//------------------------------------------------------------------------------

/// 互斥锁超时时间 (ms)
#define MUTEX_TIMEOUT_MS (50)

/* 编码器量化和滤波噪声会在硬件刹车后留下极小的非零RPM；精确浮点
   比较会让服务永久锁在ESTOP。 */
#define MOTION_STOP_RPM_EPSILON (1.0f)

/// mode 1-3 必须由上游持续刷新；超时后 STM32 本地硬停
#define MOTION_COMMAND_LEASE_MS (500u)

/// GOAL 有有效输出但无里程/航向进展时判定堵转
#define MOTION_GOAL_STALL_MS (1500u)
#define MOTION_GOAL_STALL_ARM_MS (750u)
#define MOTION_GOAL_PROGRESS_MM (1.0f)
#define MOTION_GOAL_PROGRESS_DEG (0.25f)
#define MOTION_GOAL_ACTIVE_RPM (10.0f)

/// 最小有效时间间隔 (ms)
#define MIN_DELTA_TIME_MS (1)

/// 圆周率定义
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif


//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------

/// 毫米转米
static inline float mm_to_m(float mm);

/// 处理紧急停止
static void handle_emergency_stop(MotionContext* ctx);

/// 处理直接控制
static void handle_direct_control(MotionContext* ctx, const MotionTarget* target);

/// 处理差速控制
static void handle_differential_control(MotionContext* ctx, const MotionTarget* target);

/// 处理 DRIVE（速度 + 平行转向）
static void handle_drive(MotionContext* ctx, const MotionTarget* target);

/// 处理目标型运动
static void handle_goal(MotionContext* ctx);

/// 左右轮是否已低于安全停稳阈值
static bool drive_is_stopped(const MotionContext* ctx);

static bool can_accept_motion_locked(const MotionContext* ctx);

static void abort_goal_locked(MotionContext* ctx, uint8_t newState);

static void stop_actuators(MotionContext* ctx);

static bool poll_command_lease(MotionContext* ctx, uint32_t nowMs);

static bool goal_is_stalled(const MotionContext* ctx, uint32_t nowMs);

static bool encoder_fault_detected(MotionContext* ctx,
                                   const MotorState* left,
                                   const MotorState* right,
                                   uint32_t nowMs);

/// 由底盘配置构造目标执行器配置
static MotionGoalConfig goal_cfg_from(const ChassisConfig* cfg);

/// 更新运行时状态
static void update_runtime_state(MotionContext* ctx);


//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------

int MotionContext_Init(MotionContext* ctx, MotorService* motor, SteerInstance* steer, const ChassisConfig* config)
{
    // 参数验证
    if (ctx == NULL || motor == NULL || steer == NULL || config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_INVALID_PARAM;
    }

    // 初始化结构体
    memset(ctx, 0, sizeof(MotionContext));

    // 设置服务实例
    ctx->motorService = motor;
    ctx->steerServo   = steer;

    // 复制底盘配置
    memcpy(&ctx->chassisConfig, config, sizeof(ChassisConfig));

    // 创建互斥锁
    ctx->targetMutex = osMutexNew(NULL);
    if (ctx->targetMutex == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Mutex creation failed");
        return ERR_MOTION_MUTEX_FAIL;
    }

    // 初始化为紧急停止模式
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    ctx->state.currentMode = MOTION_EMERGENCY_STOP;
    control_lease_init(&ctx->commandLease, MOTION_COMMAND_LEASE_MS);
    encoder_fault_monitor_init(&ctx->encoderMonitor[MOTOR_LEFT_WHEEL]);
    encoder_fault_monitor_init(&ctx->encoderMonitor[MOTOR_RIGHT_WHEEL]);
    ctx->urgentStatus = true;


    // 创建任务 TODO
    MotionTask_init(ctx);

    return ERR_SUCCESS;
}

MotionMode MotionService_GetCurrentMode(const MotionContext* ctx)
{
    if (ctx == NULL)
    {
        return MOTION_EMERGENCY_STOP;
    }
    MotionMode mode = MOTION_EMERGENCY_STOP;
    if (osMutexAcquire(((MotionContext*)ctx)->targetMutex, MUTEX_TIMEOUT_MS) == osOK)
    {
        mode = ctx->motionTarget.mode;
        osMutexRelease(((MotionContext*)ctx)->targetMutex);
    }
    return mode;
}

int MotionService_SetDifferentialParams(MotionContext* ctx, float linear, float angular,
                                        uint8_t seq)
{
    // 参数验证
    if (ctx == NULL || !isfinite(linear) || !isfinite(angular))
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_INVALID_PARAM;
    }

    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTION, "Mutex timeout");
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    if (!can_accept_motion_locked(ctx))
    {
        osMutexRelease(ctx->targetMutex);
        return ctx->faultLatched ? ERR_MOTION_FAULT_LATCHED : ERR_MOTION_EMERGENCY_ACTIVE;
    }

    // 参数范围校验
    const float max_linear  = ctx->chassisConfig.maxLinearVelocityMPS;
    const float max_angular = ctx->chassisConfig.maxAngularVelRad;

    const float clamped_linear  = CLAMP(linear, -max_linear, max_linear);
    const float clamped_angular = CLAMP(angular, -max_angular, max_angular);

    abort_goal_locked(ctx, MOTION_STATE_ABORTED);
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->motionTarget.mode = MOTION_DIFFERENTIAL;
    ctx->motionTarget.params.diffCtrl.linearVelocity  = clamped_linear;
    ctx->motionTarget.params.diffCtrl.angularVelocity = clamped_angular;
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    control_lease_renew(&ctx->commandLease, HAL_GetTick());

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
}

int MotionService_SetDirectControlParams(MotionContext* ctx, float left_rpm, float right_rpm,
                                         float steer_angle, uint8_t seq)
{
    // 参数验证
    if (ctx == NULL || !isfinite(left_rpm) || !isfinite(right_rpm) || !isfinite(steer_angle))
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_INVALID_PARAM;
    }

    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTION, "Mutex timeout");
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    if (!can_accept_motion_locked(ctx))
    {
        osMutexRelease(ctx->targetMutex);
        return ctx->faultLatched ? ERR_MOTION_FAULT_LATCHED : ERR_MOTION_EMERGENCY_ACTIVE;
    }

    abort_goal_locked(ctx, MOTION_STATE_ABORTED);
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->motionTarget.mode = MOTION_DIRECT_CONTROL;
    ctx->motionTarget.params.directCtrl.leftRpm = motion_clamp_rpm(left_rpm, MOTION_MAX_RPM);
    ctx->motionTarget.params.directCtrl.rightRpm = motion_clamp_rpm(right_rpm, MOTION_MAX_RPM);
    ctx->motionTarget.params.directCtrl.steerPercent =
        CLAMP(steer_angle, STEERING_PERCENT_MIN, STEERING_PERCENT_MAX);
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    control_lease_renew(&ctx->commandLease, HAL_GetTick());

    osMutexRelease(ctx->targetMutex);

    return ERR_SUCCESS;
}

int MotionService_SetDriveParams(MotionContext* ctx, float linear_vel, float steer_angle,
                                 uint8_t seq)
{
    // 参数验证
    if (ctx == NULL || !isfinite(linear_vel) || !isfinite(steer_angle))
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_INVALID_PARAM;
    }

    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTION, "Mutex timeout");
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    if (!can_accept_motion_locked(ctx))
    {
        osMutexRelease(ctx->targetMutex);
        return ctx->faultLatched ? ERR_MOTION_FAULT_LATCHED : ERR_MOTION_EMERGENCY_ACTIVE;
    }

    // 参数范围校验
    const float max_vel   = ctx->chassisConfig.maxLinearVelocityMPS;

    abort_goal_locked(ctx, MOTION_STATE_ABORTED);
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->motionTarget.mode = MOTION_DRIVE;
    ctx->motionTarget.params.driveCtrl.linearVelocity =
        CLAMP(linear_vel, -max_vel, max_vel);
    ctx->motionTarget.params.driveCtrl.steerPercent =
        CLAMP(steer_angle, STEERING_PERCENT_MIN, STEERING_PERCENT_MAX);
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    control_lease_renew(&ctx->commandLease, HAL_GetTick());

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
}

void MotionService_GetState(const MotionContext* ctx, MotionState* out_state)
{
    if (ctx == NULL || out_state == NULL)
    {
        return;
    }
    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) == osOK)
    {
        // 直接复制状态结构体
        memcpy(out_state, &ctx->state, sizeof(MotionState));
        osMutexRelease(ctx->targetMutex);
    }
}


int MotionService_EmergencyStop(MotionContext* ctx, uint8_t seq)
{
    // 参数验证
    if (ctx == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_INVALID_PARAM;
    }


    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTION, "Mutex timeout");
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    abort_goal_locked(ctx, MOTION_STATE_ABORTED);
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->cmdLeftRpm = 0.0f;
    ctx->cmdRightRpm = 0.0f;
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    control_lease_cancel(&ctx->commandLease);

    osMutexRelease(ctx->targetMutex);
    stop_actuators(ctx);
    return ERR_SUCCESS;
}

int MotionService_ClearFault(MotionContext* ctx, uint8_t seq)
{
    if (ctx == NULL)
    {
        return ERR_MOTION_INVALID_PARAM;
    }
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        return ERR_MOTION_MUTEX_TIMEOUT;
    }
    if (!drive_is_stopped(ctx))
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_CLEAR_FAILED;
    }
    ctx->faultLatched = false;
    ctx->faultCode = MOTION_ERR_NONE;
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    control_lease_cancel(&ctx->commandLease);
    ctx->commandLease.expired = false;
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    osMutexRelease(ctx->targetMutex);
    stop_actuators(ctx);
    return ERR_SUCCESS;
}

void MotionService_LatchFault(MotionContext* ctx, uint8_t faultCode)
{
    if (ctx == NULL)
    {
        return;
    }
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        stop_actuators(ctx);
        return;
    }
    abort_goal_locked(ctx, MOTION_STATE_ERROR);
    ctx->goal.error = faultCode;
    ctx->faultCode = faultCode;
    ctx->faultLatched = true;
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->cmdLeftRpm = 0.0f;
    ctx->cmdRightRpm = 0.0f;
    ctx->urgentStatus = true;
    control_lease_cancel(&ctx->commandLease);
    osMutexRelease(ctx->targetMutex);
    stop_actuators(ctx);
}


int MotionService_StartGoal(MotionContext* ctx, int16_t turnDdeg, int16_t distanceMm,
                            uint16_t speedMmps, uint8_t seq)
{
    if (ctx == NULL)
    {
        return ERR_MOTION_INVALID_PARAM;
    }
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    if (ctx->faultLatched)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_FAULT_LATCHED;
    }
    if (ctx->goal.active)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_BUSY;
    }

    if (ctx->motionTarget.mode == MOTION_EMERGENCY_STOP && !drive_is_stopped(ctx))
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_EMERGENCY_ACTIVE;
    }

    const MotionGoalTarget target = {
        .turnDdeg   = turnDdeg,
        .distanceMm = distanceMm,
        .speedMmps  = speedMmps,
    };
    const MotionGoalConfig cfg = goal_cfg_from(&ctx->chassisConfig);

    MotionGoalState candidate;
    const uint32_t now = HAL_GetTick();
    const int rc = motion_goal_start(&candidate, &target, &cfg, now);
    if (rc == 3)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_GEOMETRY;
    }
    if (rc != 0)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_INVALID_PARAM;
    }

    candidate.seq = seq;
    ctx->goal = candidate;
    ctx->motionTarget.mode = MOTION_GOAL;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    control_lease_cancel(&ctx->commandLease);
    ctx->statusSeq = seq;
    ctx->urgentStatus = true;
    ctx->goalLastProgressTick = now;
    ctx->goalLastProgressDistanceMm = 0.0f;
    ctx->goalLastProgressHeadingDeg = 0.0f;

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
}

void MotionService_AbortGoal(MotionContext* ctx, uint8_t newState)
{
    if (ctx == NULL)
    {
        return;
    }
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        return;
    }

    abort_goal_locked(ctx, newState);
    ctx->cmdLeftRpm = 0.0f;
    ctx->cmdRightRpm = 0.0f;
    ctx->urgentStatus = true;

    osMutexRelease(ctx->targetMutex);
    stop_actuators(ctx);
}

void MotionService_GetStatus(const MotionContext* ctx, motion_status_t* out)
{
    if (ctx == NULL || out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (osMutexAcquire(((MotionContext*)ctx)->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        return;
    }

    const MotionGoalState* g = &ctx->goal;

    const MotionMode mode = ctx->motionTarget.mode;
    out->seq = ctx->statusSeq;
    out->error = ctx->faultLatched ? ctx->faultCode : g->error;
    out->flags = (uint8_t)(((uint8_t)mode << MOTION_STATUS_FLAG_MODE_SHIFT) &
                           MOTION_STATUS_FLAG_MODE_MASK);
    if (ctx->faultLatched)
    {
        out->flags |= MOTION_STATUS_FLAG_FAULT;
        out->state = MOTION_STATE_ERROR;
    }
    else if (mode != MOTION_GOAL)
    {
        out->state = (mode != MOTION_EMERGENCY_STOP &&
                      (fabsf(ctx->cmdLeftRpm) > MOTION_STOP_RPM_EPSILON ||
                       fabsf(ctx->cmdRightRpm) > MOTION_STOP_RPM_EPSILON ||
                       !drive_is_stopped(ctx))) ? MOTION_STATE_RUNNING : MOTION_STATE_IDLE;
    }
    else if (!g->finished)
    {
        out->state = g->active ? MOTION_STATE_RUNNING : MOTION_STATE_IDLE;
    }
    else if (g->error != 0)
    {
        out->state = MOTION_STATE_ERROR;
    }
    else if (g->aborted)
    {
        out->state = MOTION_STATE_ABORTED;
    }
    else
    {
        out->state = MOTION_STATE_DONE;
    }

    out->xMm        = (int32_t)(ctx->state.pose.x * 1000.0f);
    out->yMm        = (int32_t)(ctx->state.pose.y * 1000.0f);
    out->thetaMdeg  = (int32_t)(motion_normalize_deg(
                                   ctx->state.pose.theta * (180.0f / M_PI)) * 1000.0f);
    out->odometerMm = (int32_t)(ctx->state.pose.distance * 1000.0f);

    out->goalDistanceMm = (int32_t)g->distanceTravelMm;
    out->goalTurnMdeg   = (int32_t)(g->headingTravelDeg * 1000.0f);

    if (g->active || g->finished)
    {
        const uint32_t endMs = g->finished ? g->finishMs : HAL_GetTick();
        out->goalDurationMs  = (endMs >= g->startMs) ? (endMs - g->startMs) : 0u;
    }

    const float radius_m = ctx->chassisConfig.wheelRadiusMM / 1000.0f;
    const float rpm_to_mmps = (2.0f * M_PI * radius_m / 60.0f) * 1000.0f;
    out->leftMmps  = (int16_t)(ctx->state.drive.leftRpm * rpm_to_mmps);
    out->rightMmps = (int16_t)(ctx->state.drive.rightRpm * rpm_to_mmps);

    if (ctx->commandLease.expired)
    {
        out->flags |= MOTION_STATUS_FLAG_LEASE_EXPIRED;
    }

    osMutexRelease(((MotionContext*)ctx)->targetMutex);
}

bool MotionService_ConsumeUrgentStatus(MotionContext* ctx)
{
    if (ctx == NULL || osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        return false;
    }
    const bool urgent = ctx->urgentStatus;
    ctx->urgentStatus = false;
    osMutexRelease(ctx->targetMutex);
    return urgent;
}

bool MotionService_ShouldReportFast(const MotionContext* ctx)
{
    const MotionMode mode = MotionService_GetCurrentMode(ctx);
    return mode == MOTION_DIRECT_CONTROL || mode == MOTION_DIFFERENTIAL ||
           mode == MOTION_DRIVE || mode == MOTION_GOAL;
}

void MotionService_Run(MotionContext* ctx)
{
    if (ctx == NULL)
    {
        return;
    }


    // 互斥锁获取目标参数
    MotionTarget current_target;
    bool leaseExpired = false;
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) == osOK)
    {
        leaseExpired = poll_command_lease(ctx, HAL_GetTick());
        // 拷贝整个结构体（确保内存布局一致）
        memcpy(&current_target, &ctx->motionTarget, sizeof(MotionTarget));
        osMutexRelease(ctx->targetMutex);
    }
    else
    {
        return;
    }

    if (leaseExpired)
    {
        stop_actuators(ctx);
    }

    // 根据模式执行控制
    switch (current_target.mode)
    {
        case MOTION_EMERGENCY_STOP:
            handle_emergency_stop(ctx);
            break;

        case MOTION_DIRECT_CONTROL:
            handle_direct_control(ctx, &current_target);
            break;

        case MOTION_DIFFERENTIAL:
            handle_differential_control(ctx, &current_target);
            break;

        case MOTION_DRIVE:
            handle_drive(ctx, &current_target);
            break;

        case MOTION_GOAL:
            handle_goal(ctx);
            break;

        default:
            break;
    }

    /* 记录运动状态（蜂鸣器：只有存在运动指令时才响，停车静音） */
    {
        const float cmd_sum = ctx->cmdLeftRpm + ctx->cmdRightRpm;
        safety_set_motion(fabsf(cmd_sum) > 0.5f, cmd_sum < 0.0f);
    }

    /* ---- 统一出口：超声波安全限速 ----
     * 所有模式的目标都在这里统一乘 scale 后下发，
     * 避免新增模式漏掉限速。EMERGENCY_STOP 已由上面直接停机。 */
    if (current_target.mode != MOTION_EMERGENCY_STOP)
    {
        float left  = ctx->cmdLeftRpm;
        float right = ctx->cmdRightRpm;

#if SAFETY_GOVERNOR_ENABLED
        const SafetyChannel* front = NULL;
        const SafetyChannel* rear  = NULL;
        Sonar_GetSafety(&front, &rear);

        /* 方向必须用**限速前**的指令判断：scale=0 时两轮目标都归零，
           若放在限速之后会丢掉方向，导致倒车时去检查前向传感器。 */
        const float linear = (left + right) * 0.5f;

        const float scale = safety_governor_apply(front, rear, &left, &right);

        /* ---- 硬停止：进入停止距离（或传感器故障）立即短接刹车 ----
         * 方向决定保护通道：前进看前向，倒车看后向。
         * 刹车后保持到距离 > SAFETY_BRAKE_RELEASE_CM 才解除（迟滞）。 */
        const SafetyChannel* guard = (linear >= 0.0f) ? front : rear;

        if (ctx->safetyBraked)
        {
            if (safety_channel_clear(guard, SAFETY_BRAKE_RELEASE_CM))
            {
                ctx->safetyBraked = false; /* 下面重新下发目标，恢复闭环 */
                LOG_INFO(LOG_MODULE_MOTION, "safety hard stop released");
            }
            else
            {
                left              = 0.0f;
                right             = 0.0f;
                ctx->cmdLeftRpm   = 0.0f;
                ctx->cmdRightRpm  = 0.0f;
            }
        }
        else if (safety_channel_blocked(guard))
        {
            MotorService_brakeAll(ctx->motorService);
            ctx->safetyBraked = true;
            left              = 0.0f;
            right             = 0.0f;
            ctx->cmdLeftRpm   = 0.0f;
            ctx->cmdRightRpm  = 0.0f;
            LOG_WARN(LOG_MODULE_MOTION, "safety hard stop: obstacle within %dcm",
                     (int)SAFETY_STOP_CM);
        }

        /* GOAL 模式下持续被挡 → 中止，避免 Agent 调用挂到硬超时 */
        if (current_target.mode == MOTION_GOAL && ctx->goal.active)
        {
            if (scale <= 0.001f)
            {
                const uint32_t now = HAL_GetTick();
                if (!ctx->goalBlocked)
                {
                    ctx->goalBlocked   = true;
                    ctx->goalBlockedMs = now;
                }
                else if ((now - ctx->goalBlockedMs) >= SAFETY_GOAL_BLOCK_ABORT_MS)
                {
                    ctx->goal.active   = false;
                    ctx->goal.finished = true;
                    ctx->goal.aborted  = true;
                    ctx->goal.finishMs = now;
                    ctx->goalBlocked   = false;
                    left               = 0.0f;
                    right              = 0.0f;
                    ctx->cmdLeftRpm    = 0.0f;
                    ctx->cmdRightRpm   = 0.0f;
                    LOG_WARN(LOG_MODULE_MOTION, "goal aborted: obstacle blocked");
                }
            }
            else
            {
                ctx->goalBlocked = false;
            }
        }
#endif

        /* 处于硬停止时不要下发目标，否则会把驱动从制动模式切回闭环 */
        if (!ctx->safetyBraked)
        {
            MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left);
            MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right);
        }
    }


    // 更新运行时状态
    update_runtime_state(ctx);

    if (goal_is_stalled(ctx, HAL_GetTick()))
    {
        LOG_ERROR(LOG_MODULE_MOTION, "goal stalled");
        MotionService_LatchFault(ctx, MOTION_ERR_STALL);
    }
}


//------------------------------------------------------------------------------
// 内部函数实现
//------------------------------------------------------------------------------

static inline float mm_to_m(const float mm)
{
    return mm / 1000.0f;
}

static bool drive_is_stopped(const MotionContext* ctx)
{
    return fabsf(ctx->state.drive.leftRpm) <= MOTION_STOP_RPM_EPSILON &&
           fabsf(ctx->state.drive.rightRpm) <= MOTION_STOP_RPM_EPSILON;
}

static bool can_accept_motion_locked(const MotionContext* ctx)
{
    if (ctx->faultLatched)
    {
        return false;
    }
    return ctx->motionTarget.mode != MOTION_EMERGENCY_STOP || drive_is_stopped(ctx);
}

static void abort_goal_locked(MotionContext* ctx, uint8_t newState)
{
    if (!ctx->goal.active)
    {
        return;
    }
    ctx->goal.active = false;
    ctx->goal.finished = true;
    ctx->goal.finishMs = HAL_GetTick();
    ctx->goal.aborted = (newState == MOTION_STATE_ABORTED);
    ctx->goal.error = (newState == MOTION_STATE_ERROR) ? MOTION_ERR_INTERNAL : MOTION_ERR_NONE;
    ctx->goalBlocked = false;
}

static void stop_actuators(MotionContext* ctx)
{
    MotorService_emergencyStop(ctx->motorService, MOTOR_LEFT_WHEEL);
    MotorService_emergencyStop(ctx->motorService, MOTOR_RIGHT_WHEEL);
    SteerService_setAngleImmediate(ctx->steerServo, ctx->chassisConfig.steerCenterAngleDeg);
}

/* Called with targetMutex held. */
static bool poll_command_lease(MotionContext* ctx, uint32_t nowMs)
{
    if (!control_lease_poll(&ctx->commandLease, nowMs))
    {
        return false;
    }
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
    ctx->cmdLeftRpm = 0.0f;
    ctx->cmdRightRpm = 0.0f;
    ctx->urgentStatus = true;
    LOG_WARN(LOG_MODULE_MOTION, "command lease expired");
    return true;
}

static bool goal_is_stalled(const MotionContext* ctx, uint32_t nowMs)
{
    if (!ctx->goal.active || ctx->safetyBraked || ctx->goalBlocked)
    {
        return false;
    }
    if ((uint32_t)(nowMs - ctx->goal.startMs) < MOTION_GOAL_STALL_ARM_MS)
    {
        return false;
    }
    if (fmaxf(fabsf(ctx->cmdLeftRpm), fabsf(ctx->cmdRightRpm)) < MOTION_GOAL_ACTIVE_RPM)
    {
        return false;
    }
    return (uint32_t)(nowMs - ctx->goalLastProgressTick) > MOTION_GOAL_STALL_MS;
}

static bool encoder_fault_detected(MotionContext* ctx,
                                   const MotorState* left,
                                   const MotorState* right,
                                   const uint32_t nowMs)
{
    const bool inhibited = ctx->motionTarget.mode == MOTION_EMERGENCY_STOP ||
                           ctx->safetyBraked || ctx->goalBlocked;
    const bool left_fault = encoder_fault_monitor_poll(
        &ctx->encoderMonitor[MOTOR_LEFT_WHEEL], nowMs,
        ctx->cmdLeftRpm, left->appliedDuty, left->velocity.rpm,
        left->position.revolutions, inhibited);
    const bool right_fault = encoder_fault_monitor_poll(
        &ctx->encoderMonitor[MOTOR_RIGHT_WHEEL], nowMs,
        ctx->cmdRightRpm, right->appliedDuty, right->velocity.rpm,
        right->position.revolutions, inhibited);
    return left_fault || right_fault;
}

static void handle_emergency_stop(MotionContext* ctx)
{
    ctx->cmdLeftRpm  = 0.0f;
    ctx->cmdRightRpm = 0.0f;

    /* MotionService_EmergencyStop() performs the hard stop on entry.  The
       200Hz run loop only maintains the safe command and steering position;
       repeating the full reset here would hide encoder motion and continually
       reset controller history. */
    SteerService_setAngleImmediate(ctx->steerServo, ctx->chassisConfig.steerCenterAngleDeg);
}

static void handle_direct_control(MotionContext* ctx, const MotionTarget* target)
{
    const float percent = CLAMP(target->params.directCtrl.steerPercent,
                                STEERING_PERCENT_MIN, STEERING_PERCENT_MAX);
    float raw_steer = 0.0f;
    if (!steering_map_percent_to_raw(percent, &raw_steer))
    {
        raw_steer = 0.0f;
    }

    // 设置电机转速（限幅，防止绕过 pyserver 直接发 MQTT 超速）
    const float left_rpm  = motion_clamp_rpm(target->params.directCtrl.leftRpm, MOTION_MAX_RPM);
    const float right_rpm = motion_clamp_rpm(target->params.directCtrl.rightRpm, MOTION_MAX_RPM);
    ctx->cmdLeftRpm  = left_rpm;
    ctx->cmdRightRpm = right_rpm;
    /* 目标由 MotionService_Run 统一限速后下发 */

    // 舵机角度 = 中位 - 相对转向角（舵机/连杆方向相反：正=右）
    SteerService_setAngleSmoothly(ctx->steerServo,
                                  ctx->chassisConfig.steerCenterAngleDeg - raw_steer);
}

static void handle_differential_control(MotionContext* ctx, const MotionTarget* target)
{
    const ChassisConfig* cfg            = &ctx->chassisConfig;
    const float          track_width_m  = mm_to_m(cfg->trackWidthMM);
    const float          wheel_radius_m = mm_to_m(cfg->wheelRadiusMM);

    float left_rpm, right_rpm;
    motion_calc_differential(
        target->params.diffCtrl.linearVelocity,
        target->params.diffCtrl.angularVelocity,
        track_width_m,
        wheel_radius_m,
        MOTION_MAX_RPM,
        &left_rpm,
        &right_rpm
    );

    // 目标由 MotionService_Run 统一限速后下发
    ctx->cmdLeftRpm  = left_rpm;
    ctx->cmdRightRpm = right_rpm;

    // 舵机归中
    SteerService_setAngleImmediate(ctx->steerServo, cfg->steerCenterAngleDeg);
}

static void handle_drive(MotionContext* ctx, const MotionTarget* target)
{
    const ChassisConfig* cfg = &ctx->chassisConfig;

    const float max_vel   = cfg->maxLinearVelocityMPS;
    const float v         = CLAMP(target->params.driveCtrl.linearVelocity, -max_vel, max_vel);
    const float percent   = CLAMP(target->params.driveCtrl.steerPercent,
                                  STEERING_PERCENT_MIN, STEERING_PERCENT_MAX);
    float equivalent_steer = 0.0f;
    float raw_steer = 0.0f;
    if (!steering_map_percent_to_equivalent(percent, &equivalent_steer) ||
        !steering_map_percent_to_raw(percent, &raw_steer))
    {
        equivalent_steer = 0.0f;
        raw_steer = 0.0f;
    }

    float left_rpm = 0.0f;
    float right_rpm = 0.0f;
    motion_calc_drive(v, equivalent_steer,
                      mm_to_m(cfg->wheelbaseMM),
                      mm_to_m(cfg->trackWidthMM),
                      mm_to_m(cfg->wheelRadiusMM),
                      MOTION_MAX_RPM,
                      &left_rpm, &right_rpm);

    // 目标由 MotionService_Run 统一限速后下发
    ctx->cmdLeftRpm  = left_rpm;
    ctx->cmdRightRpm = right_rpm;

    // 舵机 = 中位 - 转向角（正=右）
    SteerService_setAngleSmoothly(ctx->steerServo, cfg->steerCenterAngleDeg - raw_steer);
}

static MotionGoalConfig goal_cfg_from(const ChassisConfig* cfg)
{
    MotionGoalConfig g;
    g.wheelbaseMm     = cfg->wheelbaseMM;
    g.trackWidthMm    = cfg->trackWidthMM;
    g.wheelRadiusMm   = cfg->wheelRadiusMM;
    g.maxSteerDeg     = cfg->maxSteerAngleDeg;
    g.maxRpm          = MOTION_MAX_RPM;
    g.maxAngularRadps = cfg->maxAngularVelRad;
    g.decelMmps2      = 300.0f;
    g.minTurnRadiusMm = cfg->minTurnRadiusMM;
    g.allowPivot      = false;
    return g;
}

static void handle_goal(MotionContext* ctx)
{
    if (!ctx->goal.active)
    {
        /* 无活动目标时明确停车，避免沿用上一个模式的目标值 */
        ctx->cmdLeftRpm  = 0.0f;
        ctx->cmdRightRpm = 0.0f;
        return;
    }

    const MotionGoalConfig cfg = goal_cfg_from(&ctx->chassisConfig);
    const MotionGoalOutput out = motion_goal_step(&ctx->goal, &cfg, HAL_GetTick());

    if (out.finished || out.error != 0)
    {
        ctx->cmdLeftRpm  = 0.0f;
        ctx->cmdRightRpm = 0.0f;
        SteerService_setAngleImmediate(ctx->steerServo, ctx->chassisConfig.steerCenterAngleDeg);
        ctx->goal.active = false;
        return;
    }

    // 目标由 MotionService_Run 统一限速后下发
    ctx->cmdLeftRpm  = out.leftRpm;
    ctx->cmdRightRpm = out.rightRpm;

    float percent = 0.0f;
    float raw_steer = 0.0f;
    if (!out.pivot &&
        (!steering_map_equivalent_to_percent(out.steerAngleDeg, &percent) ||
         !steering_map_percent_to_raw(percent, &raw_steer)))
    {
        raw_steer = 0.0f;
    }
    const float steer = ctx->chassisConfig.steerCenterAngleDeg - raw_steer;
    SteerService_setAngleSmoothly(ctx->steerServo, steer);
}

static void update_runtime_state(MotionContext* ctx)
{
    // 获取当前时间
    const uint32_t current_time = HAL_GetTick();
    const float    delta_time   = (float)(current_time - ctx->state.timestamp) / 1000.0f; // 转换为秒

    // 获取电机状态
    const MotorState left_state  = MotorService_getState(ctx->motorService, MOTOR_LEFT_WHEEL);
    const MotorState right_state = MotorService_getState(ctx->motorService, MOTOR_RIGHT_WHEEL);

    // 获取转向状态
    const SteerState steer_state = SteerService_getState(ctx->steerServo);

    // ==================== 更新核心状态 ====================
    ctx->state.timestamp = current_time;
    // 更新当前模式
    ctx->state.currentMode = ctx->motionTarget.mode;

    // 更新驱动状态
    ctx->state.drive.leftRpm  = left_state.velocity.rpm;
    ctx->state.drive.rightRpm = right_state.velocity.rpm;

    // 更新转向状态
    ctx->state.steering.commandedAngle = steer_state.commandedAngleDeg;
    ctx->state.steering.targetAngle = steer_state.targetAngleDeg;

    // ==================== 更新运动学状态 ====================
    // 实际线速度 = 平均轮速
    const float prev_linear = ctx->state.velocity.linear;
    // 实际线速度 = 平均轮速
    ctx->state.velocity.linear = (left_state.velocity.mps + right_state.velocity.mps) / 2.0f;

    // 计算加速度
    if (delta_time > 0.001f)
    {
        ctx->state.velocity.acceleration = (ctx->state.velocity.linear - prev_linear) / delta_time;
    }

    // 实际角速度 = (右轮速 - 左轮速) / 轮距
    const float track_width_m = mm_to_m(ctx->chassisConfig.trackWidthMM);
    if (track_width_m > 0.001f)
    {
        // 避免除零
        ctx->state.velocity.angular = (right_state.velocity.mps - left_state.velocity.mps) / track_width_m;
    }

    // ==================== 更新位置状态 ====================
    // 计算轮周长
    const float wheel_circumference = 2.0f * (float)M_PI * mm_to_m(ctx->chassisConfig.wheelRadiusMM);
    // 计算轮子行程增量
    const float delta_left_revs  = left_state.position.revolutions - ctx->lastLeftRevs;
    const float delta_right_revs = right_state.position.revolutions - ctx->lastRightRevs;
    // 更新累计行程
    const float delta_left_dist  = delta_left_revs * wheel_circumference;
    const float delta_right_dist = delta_right_revs * wheel_circumference;

    ctx->state.pose.leftOdometer += delta_left_dist;
    ctx->state.pose.rightOdometer += delta_right_dist;
    // 更新累积里程
    ctx->state.pose.distance += (delta_left_dist + delta_right_dist) / 2.0f;

    ctx->lastLeftRevs  = left_state.position.revolutions;
    ctx->lastRightRevs = right_state.position.revolutions;


    if (track_width_m > 0.001f)
    {
        // 航向角变化量 = (右轮行程 - 左轮行程) / 轮距
        const float delta_theta = (delta_right_dist - delta_left_dist) / track_width_m;
        ctx->state.pose.theta += delta_theta;

        // 坐标变化量 = 平均行程
        const float avg_delta = (delta_left_dist + delta_right_dist) / 2.0f;
        ctx->state.pose.x += avg_delta * cosf(ctx->state.pose.theta);
        ctx->state.pose.y += avg_delta * sinf(ctx->state.pose.theta);
    }

    /* 目标执行器的独立累加器：用同一批轮速增量推进 */
    if (ctx->goal.active)
    {
        const MotionGoalConfig goal_cfg = goal_cfg_from(&ctx->chassisConfig);
        motion_goal_accumulate(&ctx->goal,
                               delta_left_dist * 1000.0f,
                               delta_right_dist * 1000.0f,
                               &goal_cfg);
        if (fabsf(ctx->goal.distanceTravelMm - ctx->goalLastProgressDistanceMm) >=
                MOTION_GOAL_PROGRESS_MM ||
            fabsf(ctx->goal.headingTravelDeg - ctx->goalLastProgressHeadingDeg) >=
                MOTION_GOAL_PROGRESS_DEG)
        {
            ctx->goalLastProgressTick = current_time;
            ctx->goalLastProgressDistanceMm = ctx->goal.distanceTravelMm;
            ctx->goalLastProgressHeadingDeg = ctx->goal.headingTravelDeg;
        }
    }

    if (!ctx->faultLatched &&
        encoder_fault_detected(ctx, &left_state, &right_state, current_time))
    {
        LOG_ERROR(LOG_MODULE_MOTION, "encoder no-progress fault");
        MotionService_LatchFault(ctx, MOTION_ERR_ENCODER);
    }
}
