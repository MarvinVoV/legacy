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

        default:
            break;
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
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL,
                              motion_clamp_rpm(target->params.directCtrl.leftRpm, MOTION_MAX_RPM));
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL,
                              motion_clamp_rpm(target->params.directCtrl.rightRpm, MOTION_MAX_RPM));

    // 舵机角度
    SteerService_setAngleSmoothly(ctx->steerServo, steer);
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

    // 设置电机转速
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left_rpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right_rpm);

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

    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left_rpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right_rpm);

    // 舵机 = 中位 + 转向角
    SteerService_setAngleSmoothly(ctx->steerServo, cfg->steerCenterAngleDeg + steer);
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
}
