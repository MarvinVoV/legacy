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

/// 解码转向百分比
static float decode_steer_percent(int16_t permille);

/// 解码角速度
static float decode_angular_vel(int16_t angular_vel);

/// 处理运动指令
static int process_motion_command(CmdProcessorContext* ctx, const MotionCmd* motion, uint8_t seq);


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

int CmdProcessor_ProcessCommand(CmdProcessorContext* ctx, const ControlCmd* cmd, uint8_t seq)
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
        ret = process_motion_command(ctx, &cmd->motion, seq);
    }

    // 其他指令处理预留
    // if (cmd->fields & CTRL_FIELD_SENSOR) { ... }

    osMutexRelease(ctx->mutex);
    return ret;
}


//------------------------------------------------------------------------------
// 内部函数实现
//------------------------------------------------------------------------------


static int process_motion_command(CmdProcessorContext* ctx, const MotionCmd* motion, uint8_t seq)
{
    MotionContext* motionCtx = ctx->motionContext;
    int            ret       = ERR_SUCCESS;

    /* 每个 service API 先验证、再原子提交模式+参数，禁止部分切换。 */
    switch (motion->mode)
    {
        case MOTION_EMERGENCY_STOP:
            ret = MotionService_EmergencyStop(motionCtx, seq);
            break;

        case MOTION_DIFFERENTIAL:
            {
                const float linear  = decode_linear_vel(motion->params.diffCtrl.linearVel);
                const float angular = decode_angular_vel(motion->params.diffCtrl.angularVel);
                ret = MotionService_SetDifferentialParams(motionCtx, linear, angular, seq);
            }
            break;

        case MOTION_DRIVE:
            {
                const float linear = decode_linear_vel(motion->params.driveCtrl.linearVel);
                const float steer  = decode_steer_percent(motion->params.driveCtrl.steerPercent);
                ret = MotionService_SetDriveParams(motionCtx, linear, steer, seq);
            }
            break;

        case MOTION_DIRECT_CONTROL:
            {
                const float leftRpm  = (float)motion->params.directCtrl.leftRpm;
                const float rightRpm = (float)motion->params.directCtrl.rightRpm;
                const float steer    = decode_steer_percent(motion->params.directCtrl.steerPercent);
                ret = MotionService_SetDirectControlParams(motionCtx, leftRpm, rightRpm,
                                                           steer, seq);
            }
            break;

        case MOTION_CLEAR_FAULT:
            ret = MotionService_ClearFault(motionCtx, seq);
            break;

        case MOTION_GOAL:
            {
                const GoalCtrlParam* g = &motion->params.goalCtrl;
                ret = MotionService_StartGoal(motionCtx, g->turnDdeg, g->distanceMm,
                                              g->speedMmps, seq);
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
        return ret;
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
 * @brief 解码转向百分比参数
 *
 * @param permille 原始百分比值 (0.1%步长)
 * @return float 转向百分比 (-100..100)
 */
static float decode_steer_percent(const int16_t permille)
{
    return (float)permille * 0.1f;
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
