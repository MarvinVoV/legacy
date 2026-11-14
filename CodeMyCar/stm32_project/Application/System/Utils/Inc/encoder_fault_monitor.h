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
 * Monitoring is inhibited while braking, below 0.1RPM or below 12% effort.
 * No-progress timeout is at least 1500ms, extended at very low RPM to allow
 * two commanded progress intervals (encoder quantization).
 * Returns true only after both the arm delay and no-progress timeout elapsed.
 */
bool encoder_fault_monitor_poll(EncoderFaultMonitor* monitor,
                                uint32_t nowMs,
                                float commandRpm,
                                float appliedDuty,
                                float actualRpm,
                                float revolutions,
                                bool inhibited);
