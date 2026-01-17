/*
 * queue_manager.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef SERVICES_CTRL_INC_QUEUE_MANAGER_H_
#define SERVICES_CTRL_INC_QUEUE_MANAGER_H_

#include "cmsis_os2.h"

typedef enum
{
    QUEUE_TYPE_LOG = 0x01,    // 日志
    QUEUE_TYPE_SYS,           // 系统
    QUEUE_TYPE_UART_RAW_DATA, // UART 原始数据
    QUEUE_TYPE_MAX            // 仅判断
} queue_type_t;

typedef struct
{
    queue_type_t type;
    osMessageQueueId_t queue;
} QueueHandler_t;


/**
 * @brief 注册队列管理器
 */
void QueueManager_registerHandler(queue_type_t type, osMessageQueueId_t queue);
/**
 * @brief 根据类型获取队列
 */
osMessageQueueId_t QueueManager_getQueueByType(queue_type_t type);


#endif /* SERVICES_CTRL_INC_QUEUE_MANAGER_H_ */
