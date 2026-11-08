#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "motion_status.h"
typedef struct { int dummy; } MotionContext;
typedef void* osThreadId_t;
typedef int osPriority_t;
typedef struct {const char* name;unsigned stack_size;int priority;} osThreadAttr_t;
typedef unsigned TickType_t;
typedef struct { bool full; unsigned size;void* data; } StaticQueue_t;
typedef StaticQueue_t* QueueHandle_t;
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define osPriorityHigh 3
#define osPriorityAboveNormal 2
#define PROTOCOL_TYPE_MOTION 7
#define TASK_HEALTH_MOTION 1
#define LOG_ERROR(...) ((void)0)
QueueHandle_t xQueueCreateStatic(unsigned,unsigned,uint8_t*,StaticQueue_t*);
int xQueueReceive(QueueHandle_t,void*,unsigned);
int xQueueOverwrite(QueueHandle_t,const void*);
osThreadId_t osThreadNew(void (*)(void*),void*,const osThreadAttr_t*);
uint32_t HAL_GetTick(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t*,TickType_t);
void MotionTask_doTask(void*);
void MotionService_Run(MotionContext*);
bool MotionService_ConsumeUrgentStatus(MotionContext*);
bool MotionService_ShouldReportFast(MotionContext*);
void MotionService_GetStatus(MotionContext*,motion_status_t*);
void SafetySupervisor_Heartbeat(int);
bool UART_Send_Frame(int,uint8_t,uint8_t,const uint8_t*,uint16_t);
