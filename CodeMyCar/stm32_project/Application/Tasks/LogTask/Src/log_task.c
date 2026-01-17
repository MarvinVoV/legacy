/*
 * log_task.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */
#include "pkt_protocol.h"
#include "log_task.h"

#include "cmsis_os2.h"
#include "log_manager.h"
#include "queue_manager.h"
#include "uart_handle.h"

//------------------------------------------------------------------------------
// 内部状态
//------------------------------------------------------------------------------
/// @brief 上次错误发生的时间戳（避免错误风暴）
static uint32_t last_error_ticks = 0;

/// @brief 连续错误计数（用于错误抑制）
static uint32_t error_count = 0;

//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------

/**
 * @brief 日志任务执行函数
 * @param argument 任务参数（未使用）
 */
static void LogTask_Execute(void* argument);


//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------

/**
 * @brief 初始化并启动日志任务
 *
 * @note 在系统启动阶段调用，需在日志队列创建后执行
 * @note 如果任务创建失败，会通过调试通道报告
 */
void LogTask_Init(void)
{
    // 配置任务属性结构体
    const osThreadAttr_t log_task_attributes = {
        .name = "LogTask",
        .stack_size = 1024,                // 根据需求调整
        .priority = osPriorityBelowNormal, // 低于关键任务
    };

    // 创建日志任务
    osThreadId_t task_id = osThreadNew(LogTask_Execute, NULL, &log_task_attributes);

    if (task_id == NULL)
    {
        /* 任务创建失败处理 */
        // TODO
    }
}

//------------------------------------------------------------------------------
// 任务执行函数
//------------------------------------------------------------------------------

/**
 * @brief 日志任务主循环
 *
 * 执行以下操作：
 * 1. 从日志队列获取条目
 * 2. 通过指定协议发送日志
 * 3. 释放日志条目内存
 *
 * @param argument 未使用的参数（保持为NULL）
 */
static void LogTask_Execute(void* argument)
{
    // 获取日志队列句柄
    osMessageQueueId_t log_queue = QueueManager_getQueueByType(QUEUE_TYPE_LOG);
    if (log_queue == NULL)
    {
        // 等待一段时间后重试（避免100%占用CPU）
        const uint32_t retry_delay = osKernelGetTickFreq() / 10; // 100ms
        while (log_queue == NULL)
        {
            log_queue = QueueManager_getQueueByType(QUEUE_TYPE_LOG);
            osDelay(retry_delay);
        }
    }

    log_entry_t* received_entry = NULL;
    osStatus_t   status;

    // 获取当前时间用于错误抑制 5s
    const uint32_t current_ticks              = osKernelGetTickCount();
    const uint32_t error_suppression_interval = osKernelGetTickFreq() * 5;


    while (1)
    {
        // 从队列获取日志条目（100ms 超时）
        status = osMessageQueueGet(log_queue, &received_entry, NULL, 100);
        if (status == osOK)
        {
            // 重置错误计数器
            error_count = 0;

            // TODO log it

            // 释放日志内存
            if (received_entry != NULL)
            {
                vPortFree(received_entry);
                received_entry = NULL;
            }
        }
        else if (status == osErrorTimeout)
        {
            // 空队列处理（可选的看门狗或空闲状态处理）
            #ifdef LOG_TASK_IDLE_HOOK
            log_task_idle_hook();
            #endif
        }
        else
        {
            // 错误处理：只记录但不频繁报告
            if ((current_ticks - last_error_ticks) > error_suppression_interval)
            {
                // TODO
                // DEBUG_CONSOLE_OUTPUT(NULL, "[LogTask] Queue error: %d\n", status);
                last_error_ticks = current_ticks;
                error_count      = 0;
            }
            else if (error_count++ > 10)
            {
                // 连续错误超过阈值时暂停任务
                // TODO
                // DEBUG_CONSOLE_OUTPUT(NULL, "[LogTask] Multiple errors (%u), suspending\n", error_count);
                osThreadSuspend(osThreadGetId());
                osDelay(1000); // 暂停1秒
                osThreadResume(osThreadGetId());
                error_count = 0;
            }

            // 避免高错误率下的CPU占用
            osDelay(10);
        }
    }
}
