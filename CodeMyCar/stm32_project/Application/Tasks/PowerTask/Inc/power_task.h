#ifndef CMCAR_POWER_TASK_H
#define CMCAR_POWER_TASK_H
#include "battery_service.h"
void PowerTask_Init(void);
/* Nonblocking latest sample; flags and timestamp determine validity/freshness. */
bool PowerTask_GetReading(BatteryReading *out);
#endif
