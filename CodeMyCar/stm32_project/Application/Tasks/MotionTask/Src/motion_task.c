/*
 * motion_task.c
 *
 *  Created on: Apr 23, 2025
 *      Author: marvin
 */

#include "motion_task.h"
#include "freertos_os2.h"
#include "task.h"

#define CONTROL_FREQ  200
static osThreadId_t taskHandle = NULL;

void MotionTask_init(MotionContext* motionCtx)
{
    if (motionCtx == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "MotionContext is NULL");
        return;
    }
    const osThreadAttr_t taskAttributes = {
        .name = "MotionTask",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityNormal,
    };
    taskHandle = osThreadNew(MotionTask_doTask, motionCtx, &taskAttributes);
    if (taskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Failed to create MotionTask");
        return;
    }
}

void MotionTask_doTask(void* arg)
{
    MotionContext* motionCtx = (MotionContext*)arg;

    const TickType_t xInterval = pdMS_TO_TICKS(1000 / CONTROL_FREQ);
    TickType_t xLastWakeTime = xTaskGetTickCount();


    // 预热时校准初始时间
    if (xInterval > 0)
    {
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }

    while (1)
    {
        MotionService_Run(motionCtx);

        // 精确周期延迟（误差补偿机制）
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }
}
