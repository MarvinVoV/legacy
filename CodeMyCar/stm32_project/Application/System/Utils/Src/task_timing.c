#include "task_timing.h"

#include <limits.h>
#include <string.h>

void task_timing_init(TaskTimingStats* stats, const uint32_t expectedPeriodMs,
                      const uint32_t toleranceMs, const uint32_t nowMs)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    stats->expectedPeriodMs = expectedPeriodMs;
    stats->toleranceMs = toleranceMs;
    stats->lastTickMs = nowMs;
    stats->windowStartMs = nowMs;
    stats->minIntervalMs = UINT32_MAX;
}

void task_timing_record(TaskTimingStats* stats, const uint32_t nowMs)
{
    if (stats == NULL) return;
    const uint32_t interval = nowMs - stats->lastTickMs;
    stats->lastTickMs = nowMs;
    if (stats->samples == 0u)
    {
        stats->minIntervalMs = interval;
        stats->maxIntervalMs = interval;
    }
    else
    {
        if (interval < stats->minIntervalMs) stats->minIntervalMs = interval;
        if (interval > stats->maxIntervalMs) stats->maxIntervalMs = interval;
    }
    stats->samples++;
    if (interval > stats->expectedPeriodMs + stats->toleranceMs)
    {
        stats->deadlineMisses++;
    }
}

bool task_timing_report_due(const TaskTimingStats* stats, const uint32_t nowMs,
                            const uint32_t windowMs)
{
    return stats != NULL && stats->samples > 0u &&
           (uint32_t)(nowMs - stats->windowStartMs) >= windowMs;
}

void task_timing_reset_window(TaskTimingStats* stats, const uint32_t nowMs)
{
    if (stats == NULL) return;
    stats->windowStartMs = nowMs;
    stats->samples = 0u;
    stats->deadlineMisses = 0u;
    stats->maxIntervalMs = 0u;
    stats->minIntervalMs = UINT32_MAX;
}
