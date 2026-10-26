/*
 * motion_task.c
 *
 *  Created on: Apr 23, 2025
 *      Author: marvin
 */

#include "motion_task.h"
#include "freertos_os2.h"
#include "task.h"
#include "motion_status.h"
#include "uart_handle.h"

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
    uint32_t   tick = 0;


    // 预热时校准初始时间
    if (xInterval > 0)
    {
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }

    while (1)
    {
        MotionService_Run(motionCtx);
        tick++;

        /* 状态上报：运动中 100ms，静止 1Hz。每帧都带最近 seq 与终态，
           单帧丢失可由下一帧补上。 */
        const bool report = motionCtx->goal.active ? ((tick % 20u) == 0u)
                                                   : ((tick % 200u) == 0u);
        if (report)
        {
            motion_status_t status;
            MotionService_GetStatus(motionCtx, &status);
            uint8_t payload[MOTION_STATUS_SIZE];
            if (motion_status_pack(&status, payload) == 0)
            {
                UART_Send_Frame(PROTOCOL_TYPE_MOTION, status.seq, 0,
                                payload, MOTION_STATUS_SIZE);
            }
        }

        // 精确周期延迟（误差补偿机制）
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }
}
