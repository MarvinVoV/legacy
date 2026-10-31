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


//------------------------------------------------------------------------------
// 常量定义
//------------------------------------------------------------------------------

/// 互斥锁超时时间 (ms)
#define MUTEX_TIMEOUT_MS (50)

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


    // 创建任务 TODO
    MotionTask_init(ctx);

    return ERR_SUCCESS;
}

int MotionService_SetControlMode(MotionContext* ctx, MotionMode new_mode)
{
    // 参数验证
    if (ctx == NULL || new_mode >= MOTION_MODE_MAX)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Invalid parameters");
        return ERR_MOTION_MODE_MISMATCH;
    }


    // 获取互斥锁
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTION, "Mutex timeout");
        return ERR_MOTION_MUTEX_TIMEOUT;
    }

    const MotionMode current_mode = ctx->motionTarget.mode;

    // 特殊条件检查（如急停未解除）
    if (current_mode == MOTION_EMERGENCY_STOP && new_mode != MOTION_EMERGENCY_STOP)
    {
        // 检查错误状态是否清除
        if (ctx->state.drive.leftRpm != 0 || ctx->state.drive.rightRpm != 0)
        {
            osMutexRelease(ctx->targetMutex);
            return ERR_MOTION_EMERGENCY_ACTIVE;
        }
    }

    // 执行模式切换
    if (ctx->motionTarget.mode != new_mode)
    {
        // 重置参数
        memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));
        // 执行模式切换
        ctx->motionTarget.mode = new_mode;
    }

    osMutexRelease(ctx->targetMutex);

    return ERR_SUCCESS;
}


MotionMode MotionService_GetCurrentMode(const MotionContext* ctx)
{
    if (ctx == NULL)
    {
        return MOTION_EMERGENCY_STOP;
    }
    return ctx->state.currentMode;
}

int MotionService_SetDifferentialParams(MotionContext* ctx, float linear, float angular)
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

    // 检查模式兼容性
    if (ctx->motionTarget.mode != MOTION_DIFFERENTIAL)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const float max_linear  = ctx->chassisConfig.maxLinearVelocityMPS;
    const float max_angular = ctx->chassisConfig.maxAngularVelRad;

    const float clamped_linear  = CLAMP(linear, -max_linear, max_linear);
    const float clamped_angular = CLAMP(angular, -max_angular, max_angular);

    // 设置参数
    ctx->motionTarget.params.diffCtrl.linearVelocity  = clamped_linear;
    ctx->motionTarget.params.diffCtrl.angularVelocity = clamped_angular;

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
}

int MotionService_SetDirectControlParams(MotionContext* ctx, float left_rpm, float right_rpm, float steer_angle)
{
    // 参数验证
    if (ctx == NULL || !isfinite(left_rpm) || !isfinite(right_rpm))
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

    // 检查模式兼容性
    if (ctx->motionTarget.mode != MOTION_DIRECT_CONTROL)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 设置参数
    ctx->motionTarget.params.directCtrl.leftRpm    = left_rpm;
    ctx->motionTarget.params.directCtrl.rightRpm   = right_rpm;
    ctx->motionTarget.params.directCtrl.steerAngle = steer_angle;

    osMutexRelease(ctx->targetMutex);

    return ERR_SUCCESS;
}

int MotionService_SetDriveParams(MotionContext* ctx, float linear_vel, float steer_angle)
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

    // 检查模式兼容性
    if (ctx->motionTarget.mode != MOTION_DRIVE)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const float max_vel   = ctx->chassisConfig.maxLinearVelocityMPS;
    const float max_angle = ctx->chassisConfig.maxSteerAngleDeg;

    ctx->motionTarget.params.driveCtrl.linearVelocity =
        CLAMP(linear_vel, -max_vel, max_vel);
    ctx->motionTarget.params.driveCtrl.steerAngle =
        CLAMP(steer_angle, -max_angle, max_angle);

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


int MotionService_EmergencyStop(MotionContext* ctx)
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

    // 设置紧急停止模式
    ctx->motionTarget.mode = MOTION_EMERGENCY_STOP;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));

    // 执行急停
    MotorService_emergencyStop(ctx->motorService, MOTOR_LEFT_WHEEL);
    MotorService_emergencyStop(ctx->motorService, MOTOR_RIGHT_WHEEL);

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
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

    if (ctx->goal.active)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_BUSY;
    }

    if (ctx->motionTarget.mode == MOTION_EMERGENCY_STOP &&
        (ctx->state.drive.leftRpm != 0.0f || ctx->state.drive.rightRpm != 0.0f))
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

    const int rc = motion_goal_start(&ctx->goal, &target, &cfg, HAL_GetTick());
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

    ctx->goal.seq = seq;
    ctx->motionTarget.mode = MOTION_GOAL;
    memset(&ctx->motionTarget.params, 0, sizeof(ctx->motionTarget.params));

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

    if (ctx->goal.active)
    {
        ctx->goal.active   = false;
        ctx->goal.finished = true;
        ctx->goal.finishMs = HAL_GetTick();
        ctx->goal.aborted  = (newState == MOTION_STATE_ABORTED);
        ctx->goal.error    = (newState == MOTION_STATE_ERROR) ? MOTION_ERR_INTERNAL : 0;

        MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, 0.0f);
        MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, 0.0f);
        SteerService_setAngleImmediate(ctx->steerServo, ctx->chassisConfig.steerCenterAngleDeg);
    }

    osMutexRelease(ctx->targetMutex);
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

    out->seq   = g->seq;
    out->error = g->error;
    if (!g->finished)
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

    osMutexRelease(((MotionContext*)ctx)->targetMutex);
}

void MotionService_Run(MotionContext* ctx)
{
    if (ctx == NULL)
    {
        return;
    }


    // 互斥锁获取目标参数
    MotionTarget current_target;
    if (osMutexAcquire(ctx->targetMutex, MUTEX_TIMEOUT_MS) == osOK)
    {
        // 拷贝整个结构体（确保内存布局一致）
        memcpy(&current_target, &ctx->motionTarget, sizeof(MotionTarget));
        osMutexRelease(ctx->targetMutex);
    }
    else
    {
        return;
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
}


//------------------------------------------------------------------------------
// 内部函数实现
//------------------------------------------------------------------------------

static inline float mm_to_m(const float mm)
{
    return mm / 1000.0f;
}

static void handle_emergency_stop(MotionContext* ctx)
{
    ctx->cmdLeftRpm  = 0.0f;
    ctx->cmdRightRpm = 0.0f;

    // 停止所有电机
    MotorService_emergencyStop(ctx->motorService, MOTOR_LEFT_WHEEL);
    MotorService_emergencyStop(ctx->motorService, MOTOR_RIGHT_WHEEL);

    // 快速归中
    SteerService_setAngleImmediate(ctx->steerServo, ctx->chassisConfig.steerCenterAngleDeg);
}

static void handle_direct_control(MotionContext* ctx, const MotionTarget* target)
{
    const float max_steer = ctx->chassisConfig.maxSteerAngleDeg;
    const float steer     = CLAMP(target->params.directCtrl.steerAngle,
                                  -max_steer, max_steer);

    // 设置电机转速（限幅，防止绕过 pyserver 直接发 MQTT 超速）
    const float left_rpm  = motion_clamp_rpm(target->params.directCtrl.leftRpm, MOTION_MAX_RPM);
    const float right_rpm = motion_clamp_rpm(target->params.directCtrl.rightRpm, MOTION_MAX_RPM);
    ctx->cmdLeftRpm  = left_rpm;
    ctx->cmdRightRpm = right_rpm;
    /* 目标由 MotionService_Run 统一限速后下发 */

    // 舵机角度 = 中位 - 相对转向角（舵机/连杆方向相反：正=右）
    SteerService_setAngleSmoothly(ctx->steerServo,
                                  ctx->chassisConfig.steerCenterAngleDeg - steer);
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
    const float max_angle = cfg->maxSteerAngleDeg;
    const float v         = CLAMP(target->params.driveCtrl.linearVelocity, -max_vel, max_vel);
    const float steer     = CLAMP(target->params.driveCtrl.steerAngle, -max_angle, max_angle);

    float left_rpm = 0.0f;
    float right_rpm = 0.0f;
    motion_calc_drive(v, steer,
                      mm_to_m(cfg->wheelbaseMM),
                      mm_to_m(cfg->trackWidthMM),
                      mm_to_m(cfg->wheelRadiusMM),
                      MOTION_MAX_RPM,
                      &left_rpm, &right_rpm);

    // 目标由 MotionService_Run 统一限速后下发
    ctx->cmdLeftRpm  = left_rpm;
    ctx->cmdRightRpm = right_rpm;

    // 舵机 = 中位 - 转向角（正=右）
    SteerService_setAngleSmoothly(ctx->steerServo, cfg->steerCenterAngleDeg - steer);
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

    const float steer = out.pivot ? ctx->chassisConfig.steerCenterAngleDeg
                                  : ctx->chassisConfig.steerCenterAngleDeg - out.steerAngleDeg;
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
    ctx->state.steering.actualAngle = steer_state.actualAngleDeg;
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
    }
}
