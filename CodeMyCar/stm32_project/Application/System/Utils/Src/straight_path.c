#include "straight_path.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SP_RAD_PER_DEG (0.017453292519943295f)
#define SP_DEG_PER_RAD (57.29577951308232f)
#define SP_MAX_OFFSET_DEG (1.0f)
#define SP_OFFSET_RATE_DPS (0.75f)
#define SP_END_TAPER_M (0.25f)

static float clampf(float value, float lo, float hi)
{
    return fmaxf(lo, fminf(hi, value));
}

bool straight_path_start(StraightPath* path, float originYawDeg, float distanceM)
{
    if (path == NULL) { return false; }
    memset(path, 0, sizeof(*path));
    if (!isfinite(originYawDeg) || !isfinite(distanceM) || distanceM == 0.0f)
    {
        return false;
    }
    path->active = true;
    path->direction = distanceM > 0.0f ? 1 : -1;
    path->originYawDeg = originYawDeg;
    return true;
}

bool straight_path_update(StraightPath* path, float signedDistanceM, float yawDeg)
{
    if (path == NULL || !path->active || !isfinite(signedDistanceM) ||
        !isfinite(yawDeg)) { return false; }
    /* A transaction rejects IMU reference changes before calling us. Keep
     * the relative angle unwrapped; wrapping +/-180 before averaging would
     * integrate the wrong direction across the branch cut. */
    const float heading = (yawDeg-path->originYawDeg)*SP_RAD_PER_DEG;
    if (!isfinite(heading)) { return false; }
    const float mid = 0.5f*(path->previousHeadingRad+heading);
    const float x = path->forwardM + signedDistanceM*cosf(mid);
    const float y = path->leftM + signedDistanceM*sinf(mid);
    if (!isfinite(x) || !isfinite(y)) { return false; }
    path->forwardM = x;
    path->leftM = y;
    path->maxAbsLeftM = fmaxf(path->maxAbsLeftM, fabsf(y));
    path->previousHeadingRad = heading;
    return true;
}

void straight_path_stop(StraightPath* path)
{
    if (path == NULL) { return; }
    path->active = false;
    path->headingOffsetDeg = 0.0f;
}

float straight_path_reference(StraightPath* path, float speedMps,
                              float remainingM, float dtSeconds)
{
    if (path == NULL) { return 0.0f; }
    if (!path->active || !isfinite(speedMps) || !isfinite(remainingM) ||
        !isfinite(dtSeconds) || !isfinite(path->leftM) ||
        remainingM < 0.0f || dtSeconds <= 0.0f ||
        dtSeconds > 0.2f || speedMps*path->direction <= 0.03f)
    {
        path->headingOffsetDeg = 0.0f;
        return path->originYawDeg;
    }
    const float lookaheadM = 0.25f + 0.8f*fabsf(speedMps);
    /* Reverse travel requires the opposite heading change for the same
     * left-of-line error. Direction is fixed by the accepted transaction. */
    const float desired = -path->direction*atan2f(path->leftM, lookaheadM)*SP_DEG_PER_RAD;
    const float taper = clampf(remainingM/SP_END_TAPER_M, 0.0f, 1.0f);
    const float target = clampf(desired, -SP_MAX_OFFSET_DEG, SP_MAX_OFFSET_DEG)*taper;
    const float maxStep = SP_OFFSET_RATE_DPS*dtSeconds;
    path->headingOffsetDeg += clampf(target-path->headingOffsetDeg, -maxStep, maxStep);
    return path->originYawDeg + path->headingOffsetDeg;
}
