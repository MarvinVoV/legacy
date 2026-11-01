#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    TASK_HEALTH_MOTION = 0,
    TASK_HEALTH_MOTOR_LEFT,
    TASK_HEALTH_MOTOR_RIGHT,
    TASK_HEALTH_CTRL,
    TASK_HEALTH_SERVO,
    TASK_HEALTH_COUNT
} TaskHealthId;

typedef struct
{
    volatile uint32_t lastBeatMs[TASK_HEALTH_COUNT];
    uint32_t requiredMask;
    uint32_t graceUntilMs;
    uint32_t staleMask;
    bool faultLatched;
} TaskHealthMonitor;

void task_health_init(TaskHealthMonitor* monitor, uint32_t requiredMask,
                      uint32_t nowMs, uint32_t graceMs);
void task_health_beat(TaskHealthMonitor* monitor, TaskHealthId id, uint32_t nowMs);
uint32_t task_health_check(TaskHealthMonitor* monitor, uint32_t nowMs,
                           uint32_t timeoutMs);
void task_health_clear(TaskHealthMonitor* monitor, uint32_t nowMs, uint32_t graceMs);
