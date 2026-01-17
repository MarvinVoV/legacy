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


//------------------------------------------------------------------------------
// 常量定义
//------------------------------------------------------------------------------

/// 互斥锁超时时间 (ms)
#define MUTEX_TIMEOUT_MS (50)

/// 最小有效时间间隔 (ms)
#define MIN_DELTA_TIME_MS (1)

/// 模式数量
#define MOTION_MODE_MAX (6)

/// 圆周率定义
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif


//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------

/// 毫米转米
static inline float mm_to_m(float mm);

/**
 * @brief 计算差速模型RPM
 *
 * @param linear_vel 线速度 (m/s)
 * @param angular_vel 角速度 (rad/s)
 * @param track_width_m 轮距 (m)
 * @param wheel_radius_m 轮半径 (m)
 * @param left_rpm 输出左轮RPM
 * @param right_rpm 输出右轮RPM
 */
static void calculate_differential_rpm(float  linear_vel,
                                       float  angular_vel,
                                       float  track_width_m,
                                       float  wheel_radius_m,
                                       float* left_rpm,
                                       float* right_rpm);

/// 处理紧急停止
static void handle_emergency_stop(MotionContext* ctx);

/// 处理直接控制
static void handle_direct_control(MotionContext* ctx, const MotionTarget* target);

/// 处理差速控制
static void handle_differential_control(MotionContext* ctx, const MotionTarget* target);

/// 处理前轮平行转向
static void handle_parallel_steering(MotionContext* ctx, const MotionTarget* target);

/// 处理原地旋转
static void handle_spin_in_place(MotionContext* ctx, const MotionTarget* target);

/// 处理混合转向
static void handle_mixed_steering(MotionContext* ctx, const MotionTarget* target);

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

int MotionService_SetSteerParallelParams(MotionContext* ctx, float linear_vel, float steer_angle)
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
    const MotionMode mode = ctx->motionTarget.mode;
    if (mode != MOTION_STEER_PARALLEL && mode != MOTION_MIXED_STEER)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const float max_vel   = ctx->chassisConfig.maxLinearVelocityMPS;
    const float max_angle = ctx->chassisConfig.maxSteerAngleDeg;

    const float clamped_vel   = CLAMP(linear_vel, -max_vel, max_vel);
    const float clamped_angle = CLAMP(steer_angle, -max_angle, max_angle);

    ctx->motionTarget.params.parallelSteer.linearVelocity = clamped_vel;
    ctx->motionTarget.params.parallelSteer.steerAngle     = clamped_angle;

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
}

int MotionService_SetMixedVelocityParams(MotionContext* ctx,
                                         const float    linear,
                                         const float    angular,
                                         const float    steer_angle,
                                         const float    steering_blend)
{
    // 参数验证
    if (ctx == NULL || !isfinite(linear) || !isfinite(angular) ||
        !isfinite(steer_angle) || !isfinite(steering_blend))
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
    if (ctx->motionTarget.mode != MOTION_MIXED_STEER)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const ChassisConfig* cfg = &ctx->chassisConfig;

    const float clamped_linear  = CLAMP(linear, -cfg->maxLinearVelocityMPS, cfg->maxLinearVelocityMPS);
    const float clamped_angular = CLAMP(angular, -cfg->maxAngularVelRad, cfg->maxAngularVelRad);
    const float clamped_steer   = CLAMP(steer_angle, -cfg->maxSteerAngleDeg, cfg->maxSteerAngleDeg);
    const float clamped_blend   = CLAMP(steering_blend, 0.0f, 1.0f);

    // 设置参数
    ctx->motionTarget.params.mixedCtrl.driveCtrlType                            = DRIVE_CTRL_VELOCITY;
    ctx->motionTarget.params.mixedCtrl.driveParams.velocityCtrl.linearVelocity  = clamped_linear;
    ctx->motionTarget.params.mixedCtrl.driveParams.velocityCtrl.angularVelocity = clamped_angular;
    ctx->motionTarget.params.mixedCtrl.steerAngle                               = clamped_steer;
    ctx->motionTarget.params.mixedCtrl.steeringBlend                            = clamped_blend;

    osMutexRelease(ctx->targetMutex);

    return ERR_SUCCESS;
}


int MotionService_SetMixedDutyParams(MotionContext* ctx,
                                     const float    left_duty,
                                     const float    right_duty,
                                     const float    steer_angle,
                                     const float    steering_blend)
{
    // 参数验证
    if (ctx == NULL ||
        !isfinite(left_duty) || fabsf(left_duty) > 1.0f ||
        !isfinite(right_duty) || fabsf(right_duty) > 1.0f ||
        !isfinite(steer_angle) || !isfinite(steering_blend))
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
    if (ctx->motionTarget.mode != MOTION_MIXED_STEER)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const ChassisConfig* cfg = &ctx->chassisConfig;

    const float clamped_left_duty  = CLAMP(left_duty, -1.0f, 1.0f);
    const float clamped_right_duty = CLAMP(right_duty, -1.0f, 1.0f);
    const float clamped_steer      = CLAMP(steer_angle, -cfg->maxSteerAngleDeg, cfg->maxSteerAngleDeg);
    const float clamped_blend      = CLAMP(steering_blend, 0.0f, 1.0f);

    // 设置参数
    ctx->motionTarget.params.mixedCtrl.driveCtrlType                            = DRIVE_CTRL_DUTY_CYCLE;
    ctx->motionTarget.params.mixedCtrl.driveParams.dutyCycleCtrl.leftDutyCycle  = clamped_left_duty;
    ctx->motionTarget.params.mixedCtrl.driveParams.dutyCycleCtrl.rightDutyCycle = clamped_right_duty;
    ctx->motionTarget.params.mixedCtrl.steerAngle                               = clamped_steer;
    ctx->motionTarget.params.mixedCtrl.steeringBlend                            = clamped_blend;

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


int MotionService_SetSteerAngle(MotionContext* ctx, const float angle_deg)
{
    // 参数验证
    if (ctx == NULL || !isfinite(angle_deg))
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
    const MotionMode mode = ctx->motionTarget.mode;
    if (mode != MOTION_STEER_PARALLEL && mode != MOTION_MIXED_STEER)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const float max_angle     = ctx->chassisConfig.maxSteerAngleDeg;
    const float clamped_angle = CLAMP(angle_deg, -max_angle, max_angle);

    // 设置参数
    if (mode == MOTION_STEER_PARALLEL)
    {
        ctx->motionTarget.params.parallelSteer.steerAngle = clamped_angle;
    }
    else
    {
        ctx->motionTarget.params.mixedCtrl.steerAngle = clamped_angle;
    }

    osMutexRelease(ctx->targetMutex);

    return ERR_SUCCESS;
}

int MotionService_SetSpinParams(MotionContext* ctx, float angular_vel, uint8_t rotation_point)
{
    // 参数验证
    if (ctx == NULL || !isfinite(angular_vel))
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
    if (ctx->motionTarget.mode != MOTION_SPIN_IN_PLACE)
    {
        osMutexRelease(ctx->targetMutex);
        return ERR_MOTION_MODE_MISMATCH;
    }

    // 参数范围校验
    const float max_angular     = ctx->chassisConfig.maxAngularVelRad;
    const float clamped_angular = CLAMP(angular_vel, -max_angular, max_angular);

    // 设置参数
    ctx->motionTarget.params.spinCtrl.angularVelocity = clamped_angular;
    ctx->motionTarget.params.spinCtrl.rotationPoint   = rotation_point;

    osMutexRelease(ctx->targetMutex);
    return ERR_SUCCESS;
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

        case MOTION_STEER_PARALLEL:
            handle_parallel_steering(ctx, &current_target);
            break;

        case MOTION_SPIN_IN_PLACE:
            handle_spin_in_place(ctx, &current_target);
            break;

        case MOTION_MIXED_STEER:
            handle_mixed_steering(ctx, &current_target);
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

static void calculate_differential_rpm(const float linear_vel,
                                       const float angular_vel,
                                       const float track_width_m,
                                       const float wheel_radius_m,
                                       float*      left_rpm,
                                       float*      right_rpm)
{
    // 计算左右轮线速度
    const float half_track = track_width_m / 2.0f;
    const float left_vel   = linear_vel - angular_vel * half_track;
    const float right_vel  = linear_vel + angular_vel * half_track;

    // 转换为RPM
    const float factor = 60.0f / (2.0f * (float)M_PI * wheel_radius_m);
    *left_rpm          = left_vel * factor;
    *right_rpm         = right_vel * factor;

    // 限制最大RPM
    // TODO
}

static void handle_emergency_stop(MotionContext* ctx)
{
    // 停止所有电机
    MotorService_emergencyStop(ctx->motorService, MOTOR_LEFT_WHEEL);
    MotorService_emergencyStop(ctx->motorService, MOTOR_RIGHT_WHEEL);

    // 快速归中
    SteerService_setAngleImmediate(ctx->steerServo, 90);
}

static void handle_direct_control(MotionContext* ctx, const MotionTarget* target)
{
    // 设置电机转速
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL,
                              target->params.directCtrl.leftRpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL,
                              target->params.directCtrl.rightRpm);

    // 舵机角度
    SteerService_setAngleSmoothly(ctx->steerServo, target->params.directCtrl.steerAngle);
}

static void handle_differential_control(MotionContext* ctx, const MotionTarget* target)
{
    const ChassisConfig* cfg            = &ctx->chassisConfig;
    const float          track_width_m  = mm_to_m(cfg->trackWidthMM);
    const float          wheel_radius_m = mm_to_m(cfg->wheelRadiusMM);

    // 计算差速RPM
    float left_rpm, right_rpm;
    calculate_differential_rpm(
        target->params.diffCtrl.linearVelocity,
        target->params.diffCtrl.angularVelocity,
        track_width_m,
        wheel_radius_m,
        &left_rpm,
        &right_rpm
    );

    // 设置电机转速
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left_rpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right_rpm);

    // 舵机归中
    SteerService_setAngleImmediate(ctx->steerServo, 90);
}

static void handle_parallel_steering(MotionContext* ctx, const MotionTarget* target)
{
    const ChassisConfig* cfg = &ctx->chassisConfig;

    // 参数限幅
    const float max_vel     = cfg->maxLinearVelocityMPS;
    const float clamped_vel = CLAMP(target->params.parallelSteer.linearVelocity,
                                    -max_vel, max_vel);

    const float max_angle     = cfg->maxSteerAngleDeg;
    const float clamped_angle = CLAMP(target->params.parallelSteer.steerAngle,
                                      -max_angle, max_angle);
    // 计算转速 (velocity to rpm)
    const float rpm = (clamped_vel * 60.0f) /
                      (2.0f * (float)M_PI * mm_to_m(cfg->wheelRadiusMM));

    // 设置电机转速
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, rpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, rpm);

    // 设置转向角
    SteerService_setAngleSmoothly(ctx->steerServo, clamped_angle);
}

static void handle_spin_in_place(MotionContext* ctx, const MotionTarget* target)
{
    static const float   factor         = 60.0f / (2.0f * (float)M_PI); // 预计算
    const ChassisConfig* cfg            = &ctx->chassisConfig;
    const float          track_width_m  = mm_to_m(cfg->trackWidthMM);
    const float          wheel_radius_m = mm_to_m(cfg->wheelRadiusMM);
    const float          wheelbase_m    = mm_to_m(cfg->wheelbaseMM);
    const float          angular_vel    = target->params.spinCtrl.angularVelocity;
    const uint8_t        rotation_point = target->params.spinCtrl.rotationPoint;

    float left_rpm    = 0.0f;
    float right_rpm   = 0.0f;
    float steer_angle = ctx->chassisConfig.steerCenterAngleDeg; // 默认中位
    // 运动学计算
    switch (rotation_point)
    {
        case 0: // 标准原地旋转（中心点）- 默认
        default:
            {
                // 基础计算：v = ω × (W/2)
                const float linear_vel_per_wheel = angular_vel * track_width_m / 2.0f;
                const float rpm                  = (linear_vel_per_wheel * factor) / wheel_radius_m;

                // 左右轮反向等速
                left_rpm  = -rpm;
                right_rpm = rpm;

                // 舵机归中
                steer_angle = ctx->chassisConfig.steerCenterAngleDeg;
            }
            break;
        case 1: // 前轴为旋转中心
            {
                // 计算需要的转向角度
                const float turn_angle_rad = atan2f(track_width_m / 2, wheelbase_m);
                const float turn_angle_deg = turn_angle_rad * (180.0f / (float)M_PI);

                // 计算转弯半径（前轴为支点）
                const float turn_radius  = wheelbase_m;
                const float outer_radius = sqrtf(powf(turn_radius, 2) + powf(track_width_m / 2, 2));

                // 计算轮子线速度
                const float linear_vel_per_wheel = angular_vel * outer_radius;
                const float rpm                  = (linear_vel_per_wheel * factor) / wheel_radius_m;

                // 对于前轴旋转，后轮需要差速
                left_rpm  = -rpm;
                right_rpm = rpm;

                // 设置转向角（前轮需要转向）
                steer_angle = ctx->chassisConfig.steerCenterAngleDeg + turn_angle_deg;
            }
            break;
        case 2: // 后轴为旋转中心
            {
                // 计算需要的转向角度
                const float turn_angle_rad = atan2f(track_width_m / 2, wheelbase_m);
                const float turn_angle_deg = turn_angle_rad * (180.0f / (float)M_PI);

                // 计算转弯半径（后轴为支点）
                const float turn_radius  = wheelbase_m;
                const float outer_radius = sqrtf(powf(turn_radius, 2) + powf(track_width_m / 2, 2));

                // 计算轮子线速度
                const float linear_vel_per_wheel = angular_vel * outer_radius;
                const float rpm                  = (linear_vel_per_wheel * factor) / wheel_radius_m;

                // 对于后轴旋转，前轮需要差速
                left_rpm  = rpm;
                right_rpm = -rpm;

                // 设置转向角（前轮需要转向）
                steer_angle =ctx->chassisConfig.steerCenterAngleDeg - turn_angle_deg;
            }
            break;
    }
    LOG_INFO(LOG_MODULE_MOTION,
             "SPIN: ω=%.2f, rp=%d, L=%.1f, R=%.1f, steer=%.1f",
             angular_vel, rotation_point, left_rpm, right_rpm, steer_angle);

    // 设置电机转速
    MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left_rpm);
    MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right_rpm);

    // 舵机归中
    SteerService_setAngleImmediate(ctx->steerServo, steer_angle);
}

static void handle_mixed_steering(MotionContext* ctx, const MotionTarget* target)
{
    const ChassisConfig* cfg = &ctx->chassisConfig;

    // 处理驱动控制
    switch (target->params.mixedCtrl.driveCtrlType)
    {
        case DRIVE_CTRL_VELOCITY:
            // 速度控制模式
            {
                const float linear  = target->params.mixedCtrl.driveParams.velocityCtrl.linearVelocity;
                const float angular = target->params.mixedCtrl.driveParams.velocityCtrl.angularVelocity;

                // 计算差速RPM
                float left_rpm, right_rpm;
                calculate_differential_rpm(
                    linear,
                    angular,
                    mm_to_m(cfg->trackWidthMM),
                    mm_to_m(cfg->wheelRadiusMM),
                    &left_rpm,
                    &right_rpm
                );

                // 应用混合比例
                const float blend     = target->params.mixedCtrl.steeringBlend;
                const float steer_rpm = (blend * target->params.mixedCtrl.steerAngle) / cfg->maxSteerAngleDeg;

                left_rpm -= steer_rpm;
                right_rpm += steer_rpm;

                // 设置电机转速
                MotorService_setTargetRPM(ctx->motorService, MOTOR_LEFT_WHEEL, left_rpm);
                MotorService_setTargetRPM(ctx->motorService, MOTOR_RIGHT_WHEEL, right_rpm);
            }
            break;

        case DRIVE_CTRL_DUTY_CYCLE:
            // 占空比控制模式
            {
                const float left_duty  = target->params.mixedCtrl.driveParams.dutyCycleCtrl.leftDutyCycle;
                const float right_duty = target->params.mixedCtrl.driveParams.dutyCycleCtrl.rightDutyCycle;

                // 设置开环占空比
                MotorService_setOpenLoopDutyCycle(ctx->motorService, MOTOR_LEFT_WHEEL, left_duty);
                MotorService_setOpenLoopDutyCycle(ctx->motorService, MOTOR_RIGHT_WHEEL, right_duty);
            }
            break;
    }

    // 设置转向角
    SteerService_setAngleSmoothly(ctx->steerServo, target->params.mixedCtrl.steerAngle);
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
