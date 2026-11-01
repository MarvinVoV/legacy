#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float    lastRevolutions;
    uint32_t armedAtMs;
    uint32_t lastProgressMs;
    bool     armed;
} EncoderFaultMonitor;

void encoder_fault_monitor_init(EncoderFaultMonitor* monitor);

/**
 * @brief Detect a commanded/energized wheel whose encoder reports no motion.
 *
 * Monitoring is inhibited while braking or below the command/duty thresholds.
 * Returns true only after both the arm delay and no-progress timeout elapsed.
 */
bool encoder_fault_monitor_poll(EncoderFaultMonitor* monitor,
                                uint32_t nowMs,
                                float commandRpm,
                                float appliedDuty,
                                float actualRpm,
                                float revolutions,
                                bool inhibited);
