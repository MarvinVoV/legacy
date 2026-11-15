#pragma once
#include <stdint.h>
typedef void *osThreadId_t;
typedef struct {
    const char *name;
    int priority;
    void *cb_mem, *stack_mem;
    uint32_t cb_size, stack_size;
} osThreadAttr_t;
enum { osPriorityLow = 8 };
int32_t osKernelLock(void);
int32_t osKernelRestoreLock(int32_t lock);
uint32_t osKernelGetTickFreq(void);
void osDelay(uint32_t ticks);
osThreadId_t osThreadNew(void (*task)(void *), void *arg, const osThreadAttr_t *attr);
