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
    monitor->graceStartMs = nowMs;
    monitor->graceMs = graceMs;
    monitor->graceActive = graceMs != 0u;
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
    if (monitor == NULL || monitor->faultLatched)
    {
        return monitor != NULL ? monitor->staleMask : 0u;
    }
    if (monitor->graceActive)
    {
        if ((uint32_t)(nowMs - monitor->graceStartMs) < monitor->graceMs)
        {
            return monitor->staleMask;
        }
        /* Expired grace cannot become active again at the signed half-wrap. */
        monitor->graceActive = false;
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
    monitor->graceStartMs = nowMs;
    monitor->graceMs = graceMs;
    monitor->graceActive = graceMs != 0u;
    for (unsigned i = 0u; i < TASK_HEALTH_COUNT; i++)
    {
        monitor->lastBeatMs[i] = nowMs;
    }
}
