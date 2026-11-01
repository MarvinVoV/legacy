#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "task_timing.h"

int main(void)
{
    TaskTimingStats stats;
    task_timing_init(&stats, 5u, 2u, UINT32_MAX - 3u);
    task_timing_record(&stats, 1u); /* wrap-safe 5ms */
    task_timing_record(&stats, 6u);
    task_timing_record(&stats, 14u); /* 8ms exceeds 7ms budget */
    assert(stats.samples == 3u);
    assert(stats.minIntervalMs == 5u);
    assert(stats.maxIntervalMs == 8u);
    assert(stats.deadlineMisses == 1u);
    assert(task_timing_report_due(&stats, 10001u, 10000u));
    task_timing_reset_window(&stats, 10001u);
    assert(stats.samples == 0u && stats.deadlineMisses == 0u);
    puts("task_timing: all tests passed");
    return 0;
}
