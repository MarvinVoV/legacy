#pragma once
#include <stdint.h>
typedef void* osMutexId_t;
typedef uint32_t osStatus_t;
#define osWaitForever UINT32_MAX
#define osOK 0u
osMutexId_t osMutexNew(const void* attr);
osStatus_t osMutexAcquire(osMutexId_t mutex, uint32_t timeout);
osStatus_t osMutexRelease(osMutexId_t mutex);
