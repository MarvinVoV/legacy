#include "task_health.h"

#include <string.h>

void task_health_init(TaskHealthMonitor* monitor, const uint32_t requiredMask,
                      const uint32_t nowMs, const uint32_t graceMs)
{
    if (monitor == NULL)
    {
        return;
    }
    memset(monitor, 0, sizeof(*monitor));
    monitor->requiredMask = requiredMask;
    monitor->graceUntilMs = nowMs + graceMs;
    for (unsigned i = 0u; i < TASK_HEALTH_COUNT; i++)
    {
        monitor->lastBeatMs[i] = nowMs;
    }
}

void task_health_beat(TaskHealthMonitor* monitor, const TaskHealthId id,
                      const uint32_t nowMs)
{
    if (monitor == NULL || id >= TASK_HEALTH_COUNT)
    {
        return;
    }
    monitor->lastBeatMs[id] = nowMs;
}

uint32_t task_health_check(TaskHealthMonitor* monitor, const uint32_t nowMs,
                           const uint32_t timeoutMs)
{
    if (monitor == NULL || monitor->faultLatched ||
        (int32_t)(nowMs - monitor->graceUntilMs) < 0)
    {
        return monitor != NULL ? monitor->staleMask : 0u;
    }

    uint32_t stale = 0u;
    for (unsigned i = 0u; i < TASK_HEALTH_COUNT; i++)
    {
        const uint32_t bit = 1u << i;
        if ((monitor->requiredMask & bit) != 0u &&
            (uint32_t)(nowMs - monitor->lastBeatMs[i]) > timeoutMs)
        {
            stale |= bit;
        }
    }
    if (stale != 0u)
    {
        monitor->staleMask = stale;
        monitor->faultLatched = true;
    }
    return stale;
}

void task_health_clear(TaskHealthMonitor* monitor, const uint32_t nowMs,
                       const uint32_t graceMs)
{
    if (monitor == NULL)
    {
        return;
    }
    monitor->staleMask = 0u;
    monitor->faultLatched = false;
    monitor->graceUntilMs = nowMs + graceMs;
    for (unsigned i = 0u; i < TASK_HEALTH_COUNT; i++)
    {
        monitor->lastBeatMs[i] = nowMs;
    }
}
