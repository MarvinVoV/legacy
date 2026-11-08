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
#include "safety_supervisor.h"
#include "queue.h"

#define CONTROL_FREQ  200
static osThreadId_t taskHandle = NULL;
static StaticQueue_t statusQueueControl;
static uint8_t statusQueueStorage[sizeof(motion_status_t)];
static QueueHandle_t statusQueue;
/* Debugger-readable timing evidence; not a real-time scheduling guarantee. */
static volatile uint32_t s_motionMaxGapMs;
static volatile uint32_t s_motionTxFailures;

static void MotionStatusTask_doTask(void* arg)
{
    (void)arg;
    motion_status_t status;
    for (;;)
    {
        if (xQueueReceive(statusQueue, &status, portMAX_DELAY) == pdTRUE)
        {
            uint8_t payload[MOTION_STATUS_SIZE];
            if (motion_status_pack(&status, payload) != 0 ||
                !UART_Send_Frame(PROTOCOL_TYPE_MOTION, status.seq, 0,
                                 payload, MOTION_STATUS_SIZE))
            {
                ++s_motionTxFailures;
            }
        }
    }
}

void MotionTask_init(MotionContext* motionCtx)
{
    if (motionCtx == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "MotionContext is NULL");
        return;
    }
    statusQueue = xQueueCreateStatic(1u, sizeof(motion_status_t),
                                     statusQueueStorage, &statusQueueControl);
    const osThreadAttr_t statusAttributes = {
        .name = "MotionStatusTask",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityAboveNormal,
    };
    if (statusQueue == NULL || osThreadNew(MotionStatusTask_doTask, NULL,
                                          &statusAttributes) == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTION, "Motion status task creation failed");
        return;
    }
    const osThreadAttr_t taskAttributes = {
        .name = "MotionTask",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityHigh,
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
    uint32_t lastControlMs = HAL_GetTick();


    // 预热时校准初始时间
    if (xInterval > 0)
    {
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }

    while (1)
    {
        const uint32_t now = HAL_GetTick();
        const uint32_t gap = now - lastControlMs;
        if (gap > s_motionMaxGapMs) { s_motionMaxGapMs = gap; }
        lastControlMs = now;
        MotionService_Run(motionCtx);
        SafetySupervisor_Heartbeat(TASK_HEALTH_MOTION);
        tick++;

        /* 状态上报：运动中 100ms，静止 1Hz。每帧都带最近 seq 与终态，
           单帧丢失可由下一帧补上。 */
        const bool urgent = MotionService_ConsumeUrgentStatus(motionCtx);
        const bool fast = MotionService_ShouldReportFast(motionCtx);
        const bool report = urgent || (fast ? ((tick % 20u) == 0u)
                                            : ((tick % 200u) == 0u));
        if (report)
        {
            motion_status_t status;
            MotionService_GetStatus(motionCtx, &status);
            /* Latest state wins. UART lock/DMA waits run only in the lower
             * priority reporter; the 200Hz control task never waits for TX. */
            xQueueOverwrite(statusQueue, &status);
        }

        // 精确周期延迟（误差补偿机制）
        vTaskDelayUntil(&xLastWakeTime, xInterval);
    }
}
