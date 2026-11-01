#include "encoder_fault_monitor.h"

#include <math.h>
#include <string.h>

#define ENCODER_MONITOR_COMMAND_RPM       15.0f
#define ENCODER_MONITOR_DUTY              0.12f
#define ENCODER_MONITOR_ACTUAL_RPM        3.0f
#define ENCODER_MONITOR_PROGRESS_REVS     0.002f
#define ENCODER_MONITOR_ARM_MS            750u
#define ENCODER_MONITOR_NO_PROGRESS_MS    1500u

void encoder_fault_monitor_init(EncoderFaultMonitor* monitor)
{
    if (monitor != NULL)
    {
        memset(monitor, 0, sizeof(*monitor));
    }
}

bool encoder_fault_monitor_poll(EncoderFaultMonitor* monitor,
                                const uint32_t nowMs,
                                const float commandRpm,
                                const float appliedDuty,
                                const float actualRpm,
                                const float revolutions,
                                const bool inhibited)
{
    if (monitor == NULL)
    {
        return false;
    }

    const bool should_monitor = !inhibited &&
        fabsf(commandRpm) >= ENCODER_MONITOR_COMMAND_RPM &&
        fabsf(appliedDuty) >= ENCODER_MONITOR_DUTY;

    if (!should_monitor)
    {
        monitor->armed = false;
        monitor->lastRevolutions = revolutions;
        monitor->armedAtMs = nowMs;
        monitor->lastProgressMs = nowMs;
        return false;
    }

    if (!monitor->armed)
    {
        monitor->armed = true;
        monitor->lastRevolutions = revolutions;
        monitor->armedAtMs = nowMs;
        monitor->lastProgressMs = nowMs;
        return false;
    }

    if (fabsf(revolutions - monitor->lastRevolutions) >=
        ENCODER_MONITOR_PROGRESS_REVS)
    {
        monitor->lastRevolutions = revolutions;
        monitor->lastProgressMs = nowMs;
        return false;
    }

    if (fabsf(actualRpm) >= ENCODER_MONITOR_ACTUAL_RPM)
    {
        monitor->lastProgressMs = nowMs;
        return false;
    }

    return (uint32_t)(nowMs - monitor->armedAtMs) > ENCODER_MONITOR_ARM_MS &&
           (uint32_t)(nowMs - monitor->lastProgressMs) >
               ENCODER_MONITOR_NO_PROGRESS_MS;
}
