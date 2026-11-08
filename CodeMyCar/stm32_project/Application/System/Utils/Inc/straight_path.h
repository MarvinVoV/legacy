#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Local frame: x is the initial vehicle-forward axis, y is left-positive.
 * Encoder average supplies signed distance; IMU yaw is continuous CCW-positive.
 * This is relative dead reckoning, not an external position measurement. */
typedef struct
{
    bool active;
    int8_t direction;
    float originYawDeg;
    float previousHeadingRad;
    float forwardM;
    float leftM;
    float maxAbsLeftM;
    float headingOffsetDeg;
} StraightPath;

bool straight_path_start(StraightPath* path, float originYawDeg, float distanceM);
bool straight_path_update(StraightPath* path, float signedDistanceM, float yawDeg);
void straight_path_stop(StraightPath* path);

/* Bounded and slew-limited outer-loop heading reference. remainingM is an
 * unsigned path length; near the endpoint the offset tapers back to zero. */
float straight_path_reference(StraightPath* path, float speedMps,
                              float remainingM, float dtSeconds);
