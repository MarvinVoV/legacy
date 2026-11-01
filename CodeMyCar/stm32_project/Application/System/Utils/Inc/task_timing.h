#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t expectedPeriodMs;
    uint32_t toleranceMs;
    uint32_t lastTickMs;
    uint32_t windowStartMs;
    uint32_t samples;
    uint32_t deadlineMisses;
    uint32_t maxIntervalMs;
    uint32_t minIntervalMs;
} TaskTimingStats;

void task_timing_init(TaskTimingStats* stats, uint32_t expectedPeriodMs,
                      uint32_t toleranceMs, uint32_t nowMs);
void task_timing_record(TaskTimingStats* stats, uint32_t nowMs);
bool task_timing_report_due(const TaskTimingStats* stats, uint32_t nowMs,
                            uint32_t windowMs);
void task_timing_reset_window(TaskTimingStats* stats, uint32_t nowMs);
