#pragma once

#include "motion_service.h"
#include "task_health.h"

/** Start the high-priority task-health supervisor and hardware watchdog. */
int SafetySupervisor_Init(MotionContext* motionContext);

/** Non-blocking heartbeat for a required control task. */
void SafetySupervisor_Heartbeat(TaskHealthId id);

/** Low-priority diagnostics only: snapshots before formatting/UART output. */
void SafetySupervisor_ReportTiming(void);
