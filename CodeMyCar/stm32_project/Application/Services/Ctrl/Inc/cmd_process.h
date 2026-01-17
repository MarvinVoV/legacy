/*
 * cmd_process.h
 *
 *  Created on: Mar 30, 2025
 *      Author: marvin
 */

#ifndef SERVICES_CTRL_INC_CMD_PROCESS_H_
#define SERVICES_CTRL_INC_CMD_PROCESS_H_

#include "ctrl_protocol.h"
#include "motion_service.h"
//------------------------------------------------------------------------------
// 类型定义
//------------------------------------------------------------------------------

typedef struct
{
    MotionContext* motionContext; ///< 运动控制上下文
    osMutexId_t    mutex;         ///< 互斥锁
} CmdProcessorContext;

//------------------------------------------------------------------------------
// 公共API
//------------------------------------------------------------------------------

/**
 * @brief 初始化指令处理器
 *
 * @param[in] ctx 指令处理器上下文
 * @param[in] motionContext 运动控制上下文
 *
 * @retval ERR_SUCCESS 初始化成功
 * @retval ERR_CMD_INVALID_PARAM 无效参数
 * @retval ERR_CMD_MUTEX_FAIL 互斥锁创建失败
 */
int CmdProcessor_Init(CmdProcessorContext* ctx, MotionContext* motionContext);

/**
 * @brief 处理控制指令
 *
 * @param[in] ctx 指令处理器上下文
 * @param[in] cmd 控制指令指针
 *
 * @retval ERR_SUCCESS 处理成功
 * @retval ERR_CMD_INVALID_PARAM 无效参数
 * @retval ERR_CMD_MUTEX_TIMEOUT 互斥锁超时
 * @retval ERR_CMD_MODE_TRANSITION 模式转换失败
 * @retval ERR_CMD_EXECUTION_FAIL 指令执行失败
 */
int CmdProcessor_ProcessCommand(CmdProcessorContext* ctx, const ControlCmd* cmd);


#endif /* SERVICES_CTRL_INC_CMD_PROCESS_H_ */
