#pragma once
#include <stdint.h>
typedef void *osMessageQueueId_t;
typedef void *osMemoryPoolId_t;
typedef enum { osKernelReady, osKernelRunning } osKernelState_t;
typedef enum { osOK=0, osErrorResource=-3 } osStatus_t;
osKernelState_t osKernelGetState(void);
uint32_t osKernelGetTickCount(void);
osMessageQueueId_t osMessageQueueNew(uint32_t,uint32_t,const void*);
osStatus_t osMessageQueuePut(osMessageQueueId_t,const void*,uint8_t,uint32_t);
