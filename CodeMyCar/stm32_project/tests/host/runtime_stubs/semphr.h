#pragma once
#include "FreeRTOS.h"
typedef struct { unsigned count; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
void xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *wake);
