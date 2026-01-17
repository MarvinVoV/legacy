/*
 * motor_task.c
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */

#include "stm32h7xx_hal.h"
#include "cmsis_os2.h"
#include "motor_task.h"
#include "ctrl_protocol.h"
#include "log_manager.h"
#include "motor_service.h"
#include "freertos_os2.h"
#include "task.h"

#define CONTROL_FREQ_FIXED   (200)   // 固定控制频率 (Hz) = 5ms

static osThreadId_t leftInstanceTaskHandle  = NULL;
static osThreadId_t rightInstanceTaskHandle = NULL;

void MotorTask_init(MotorService* motorService)
{
    if (motorService == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "MotorService is NULL");
        return;
    }
    const osThreadAttr_t leftInstanceAttributes = {
        .name = "MotorTask_Left",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityNormal,
    };
    const osThreadAttr_t rightInstanceAttributes = {
        .name = "MotorTask_Left",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityNormal,
    };

    leftInstanceTaskHandle = osThreadNew(MotorTask_doTask, &motorService->instances[0], &leftInstanceAttributes);
    if (leftInstanceTaskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Failed to create MotorTask_Left");
    }
    motorService->leftInstanceTaskHandle = leftInstanceTaskHandle;

    rightInstanceTaskHandle = osThreadNew(MotorTask_doTask, &motorService->instances[1], &rightInstanceAttributes);
    if (rightInstanceTaskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Failed to create MotorTask_Right");
    }
    motorService->rightInstanceTaskHandle = rightInstanceTaskHandle;
}

void MotorTask_doTask(void* arg)
{
    MotorInstance* motorInstance = (MotorInstance*)arg;
    // 固定5ms间隔
    const TickType_t xFixedInterval = pdMS_TO_TICKS(30); // ms

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        // 执行状态更新（确保原子性）
        taskENTER_CRITICAL();
        MotorService_updateState(motorInstance);
        taskEXIT_CRITICAL();

        // 精确周期延迟（带自动补偿）
        vTaskDelayUntil(&xLastWakeTime, xFixedInterval);
    }
}
