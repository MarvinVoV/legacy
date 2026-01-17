/*
 * servo_task.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#include "servo_task.h"
#include "cmsis_os.h"
#include "log_manager.h"
#include "servo_service.h"

static osThreadId_t servoTaskHandle = NULL;

void ServoTask_init(SteerInstance* instance)
{
    const osThreadAttr_t servoTaskAttributes = {
        .name = "ServoTask",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityNormal,
    };
    servoTaskHandle = osThreadNew(ServoTask_doTask, instance, &servoTaskAttributes);
    if (servoTaskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Failed to create ServoTask");
        return;
    }
}

void ServoTask_doTask(void* pvParameters)
{
    SteerInstance* instance = (SteerInstance*)pvParameters;
    uint32_t lastWakeTime = osKernelGetTickCount(); // 获取初始基准时间

    while (1)
    {
        /* 执行控制更新 */
        SteerService_update(instance);

        /* 精确延时到指定时间点（单参数调用） */
        osDelayUntil(lastWakeTime + pdMS_TO_TICKS(instance->config.updateIntervalMs));

        /* 更新基准时间 */
        lastWakeTime = osKernelGetTickCount();
    }
}
