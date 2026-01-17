/*
 * cmd_process.c
 *
 *  Created on: Mar 30, 2025
 *      Author: marvin
 */
#include <string.h>
#include "cmd_process.h"
#include "error_code.h"
#include "log_manager.h"
#include "ctrl_protocol.h"

//------------------------------------------------------------------------------
// 常量定义
//------------------------------------------------------------------------------

/// 互斥锁超时时间 (ms)
#define CMD_MUTEX_TIMEOUT_MS (50)

/// 最小有效线速度 (m/s)
#define MIN_LINEAR_VELOCITY (0.001f)

/// 最小有效角速度 (rad/s)
#define MIN_ANGULAR_VELOCITY (0.001f)


//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------

/// 解码线速度
static float decode_linear_vel(int16_t value);

/// 解码转向角
static float decode_steer_angle(int16_t deg);

/// 解码角速度
static float decode_angular_vel(int16_t angular_vel);

/// 解码占空比
static float decode_duty_cycle(int16_t duty);

/// 处理运动指令
static int process_motion_command(CmdProcessorContext* ctx, const MotionCmd* motion);


//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------


int CmdProcessor_Init(CmdProcessorContext* ctx, MotionContext* motionContext)
{
    // 参数验证
    if (ctx == NULL || motionContext == NULL)
    {
        LOG_ERROR(LOG_MODULE_CMD, "Invalid parameters");
        return ERR_CMD_INVALID_PARAM;
    }


    // 初始化结构体
    memset(ctx, 0, sizeof(CmdProcessorContext));

    // 设置运动上下文
    ctx->motionContext = motionContext;

    // 创建互斥锁
    ctx->mutex = osMutexNew(NULL);
    if (ctx->mutex == NULL)
    {
        LOG_ERROR(LOG_MODULE_CMD, "Mutex creation failed");
        return ERR_CMD_MUTEX_FAIL;
    }

    return ERR_SUCCESS;
}

int CmdProcessor_ProcessCommand(CmdProcessorContext* ctx, const ControlCmd* cmd)
{
    // 参数验证
    if (ctx == NULL || cmd == NULL)
    {
        LOG_ERROR(LOG_MODULE_CMD, "Invalid parameters");
        return ERR_CMD_INVALID_PARAM;
    }

    // 获取互斥锁
    if (osMutexAcquire(ctx->mutex, CMD_MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_CMD, "Mutex timeout");
        return ERR_CMD_MUTEX_TIMEOUT;
    }

    int ret = ERR_SUCCESS;
    // 处理运动指令
    if (cmd->fields & CTRL_FIELD_MOTION)
    {
        ret = process_motion_command(ctx, &cmd->motion);
    }

    // 其他指令处理预留
    // if (cmd->fields & CTRL_FIELD_SENSOR) { ... }

    osMutexRelease(ctx->mutex);
    return ret;
}


//------------------------------------------------------------------------------
// 内部函数实现
//------------------------------------------------------------------------------


static int process_motion_command(CmdProcessorContext* ctx, const MotionCmd* motion)
{
    MotionContext* motionCtx = ctx->motionContext;
    int            ret       = ERR_SUCCESS;

    // 模式切换前检查
    if (motion->mode != MOTION_EMERGENCY_STOP)
    {
        ret = MotionService_SetControlMode(motionCtx, motion->mode);
        if (ret != ERR_SUCCESS)
        {
            LOG_ERROR(LOG_MODULE_CMD, "Mode switch failed: 0x%02X, error: 0x%04X", motion->mode, ret);
            return ERR_MOTION_MODE_TRANSITION;
        }
    }

    // 模式分派处理
    switch (motion->mode)
    {
        case MOTION_EMERGENCY_STOP:
            ret = MotionService_EmergencyStop(motionCtx);
            break;

        case MOTION_DIFFERENTIAL:
            {
                const float linear  = decode_linear_vel(motion->params.diffCtrl.linearVel);
                const float angular = decode_angular_vel(motion->params.diffCtrl.angularVel);
                ret                 = MotionService_SetDifferentialParams(motionCtx, linear, angular);
            }
            break;

        case MOTION_STEER_PARALLEL:
            {
                const float linear = decode_linear_vel(motion->params.steerParallelCtrl.linearVel);
                const float steer  = decode_steer_angle(motion->params.steerParallelCtrl.steerAngle);
                ret                = MotionService_SetSteerParallelParams(motionCtx, linear, steer);
            }
            break;

        case MOTION_DIRECT_CONTROL:
            {
                const float leftRpm  = (float)motion->params.directCtrl.leftRpm;
                const float rightRpm = (float)motion->params.directCtrl.rightRpm;
                const float steer    = decode_steer_angle(motion->params.directCtrl.steerAngle);
                ret                  = MotionService_SetDirectControlParams(motionCtx, leftRpm, rightRpm, steer);
            }
            break;

        case MOTION_MIXED_STEER:
            {
                const MixedCtrlParam* mixed = &motion->params.mixedCtrl;
                const float           steer = decode_steer_angle(mixed->steerAngle);
                const float           gain  = (float)mixed->differentialGain / 100.0f; // 百分比转0.0~1.0

                if (mixed->driveCtrlType == DRIVE_CTRL_VELOCITY)
                {
                    // 速度控制模式
                    const float linear = decode_linear_vel(mixed->driveParams.velocityCtrl.linearVel);
                    const float angular = decode_angular_vel(mixed->driveParams.velocityCtrl.angularVel);
                    ret = MotionService_SetMixedVelocityParams(ctx->motionContext, linear, angular, steer, gain);
                }
                else
                {
                    // 占空比控制模式
                    const float left_duty  = decode_duty_cycle(mixed->driveParams.dutyCycleCtrl.leftDutyCycle);
                    const float right_duty = decode_duty_cycle(mixed->driveParams.dutyCycleCtrl.rightDutyCycle);
                    // 转换为占空比系数 (-1.0 ~ 1.0)
                    const float left_duty_factor = left_duty / 100.0f;
                    const float right_duty_factor = right_duty / 100.0f;
                    ret = MotionService_SetMixedDutyParams(ctx->motionContext, left_duty_factor, right_duty_factor,
                                                           steer, gain);
                }
            }
            break;

        case MOTION_SPIN_IN_PLACE:
            {
                const float   angularVel    = decode_angular_vel(motion->params.spinCtrl.angularVel);
                const uint8_t rotationPoint = motion->params.spinCtrl.rotationPoint;
                ret                         = MotionService_SetSpinParams(motionCtx, angularVel, rotationPoint);
            }
            break;

        default:
            LOG_ERROR(LOG_MODULE_CMD, "Unknown motion mode: 0x%02X", motion->mode);
            ret = ERR_MOTION_INVALID_MODE;
            break;
    }

    // 处理执行结果
    if (ret != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_CMD, "Command execute failed: mode=0x%02X, error=0x%04X", motion->mode, ret);
        return ERR_CMD_EXECUTION_FAIL;
    }

    return ERR_SUCCESS;
}

/**
 * @brief 解码线速度参数
 *
 * @param value 原始值 (0.001 m/s步长)
 * @return float 实际线速度 (m/s)
 */
static float decode_linear_vel(const int16_t value)
{
    return (float)value * 0.001f; // 0.001 m/s分辨率
}

/**
 * @brief 解码转向角参数
 *
 * @param deg 原始转向角值 (0.1度步长)
 * @return float 实际转向角 (度)
 *
 * @details 将接收到的整型转向角参数转换为浮点型角度值。
 *          输入值以0.1度为步长，即值10表示1.0度，100表示10.0度。
 */
static float decode_steer_angle(const int16_t deg)
{
    return (float)deg * 0.1f; // 0.1度分辨率
}

/**
 * @brief 解码角速度参数
 *
 * @param angular_vel 原始值 (0.0644 rad/s步长)
 * @return float 实际角速度 (rad/s)
 */
static float decode_angular_vel(const int16_t angular_vel)
{
    return (float)angular_vel * 0.0644f; // 0.0644 rad/s分辨率
}

/**
 * @brief 解码占空比参数
 *
 * @param duty 原始占空比值 (0.1%步长，范围-1000~1000)
 * @return float 实际占空比百分比 (-100.0% ~ 100.0%)
 *
 * @details 将接收到的整型占空比参数转换为浮点型百分比值。
 *          输入值以0.1%为步长，即值-1000表示-100.0%，1000表示100.0%。
 */
static float decode_duty_cycle(const int16_t duty)
{
    // 转换为百分比 (-100.0% ~ 100.0%)
    return (float)duty / 10.0f;
}
