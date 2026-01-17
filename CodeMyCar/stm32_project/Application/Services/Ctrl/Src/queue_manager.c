/*
 * queue_manager.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#include <string.h>
#include "cmsis_os.h"
#include "queue_manager.h"

#define MAX_HANDLERS (8)

static QueueHandler_t handlers[MAX_HANDLERS];
static uint32_t handler_count = 0;

void QueueManager_registerHandler(const queue_type_t type, osMessageQueueId_t queue)
{
    if (handler_count < QUEUE_TYPE_MAX)
    {
        handlers[handler_count].type = type;
        handlers[handler_count].queue = queue;
        handler_count++;
    }
}

osMessageQueueId_t QueueManager_getQueueByType(const queue_type_t type)
{
    for (uint32_t i = 0; i < handler_count; ++i)
    {
        if (handlers[i].type == type)
        {
            return handlers[i].queue;
        }
    }
    return NULL; // 未找到对应的队列
}
