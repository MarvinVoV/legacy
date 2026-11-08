#pragma once
#include <stdint.h>
typedef void* osMutexId_t;
typedef void* osThreadId_t;
typedef struct { const char* name; uint32_t attr_bits; void* cb_mem; uint32_t cb_size; } osMutexAttr_t;
#define osMutexRecursive 1u
#define osMutexPrioInherit 2u
typedef uint32_t osStatus_t;
#define osWaitForever UINT32_MAX
#define osOK 0u
osMutexId_t osMutexNew(const void* attr);
osStatus_t osMutexAcquire(osMutexId_t mutex, uint32_t timeout);
osStatus_t osMutexRelease(osMutexId_t mutex);
