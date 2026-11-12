#pragma once
#include <stddef.h>
void *pvPortMalloc(size_t);
void vPortFree(void*);
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
