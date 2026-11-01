#include "steering_map.h"

#include <math.h>
#include <stddef.h>

typedef struct
{
    float percent;
    float rawDeg;
} SteeringMapPoint;

static const SteeringMapPoint kLeftMap[] = {
    {0.0f, 0.0f}, {25.0f, 15.0f}, {50.0f, 30.0f},
    {75.0f, 45.0f}, {100.0f, 60.0f},
};

static const SteeringMapPoint kRightMap[] = {
    {0.0f, 0.0f}, {25.0f, 10.0f}, {50.0f, 15.0f},
    {75.0f, 24.0f}, {100.0f, 32.0f},
};

static bool interpolate(const SteeringMapPoint* map, size_t count,
                        float percent, float* raw_deg)
{
    if (map == NULL || count < 2u || raw_deg == NULL ||
        !isfinite(percent) || percent < 0.0f || percent > 100.0f)
    {
        return false;
    }

    for (size_t i = 1u; i < count; ++i)
    {
        if (percent <= map[i].percent)
        {
            const float width = map[i].percent - map[i - 1u].percent;
            const float ratio = (percent - map[i - 1u].percent) / width;
            *raw_deg = map[i - 1u].rawDeg +
                       ratio * (map[i].rawDeg - map[i - 1u].rawDeg);
            return true;
        }
    }
    return false;
}

bool steering_map_percent_to_raw(const float percent, float* raw_deg)
{
    if (raw_deg == NULL || !isfinite(percent) ||
        percent < STEERING_PERCENT_MIN || percent > STEERING_PERCENT_MAX)
    {
        return false;
    }

    const bool right = percent > 0.0f;
    const float magnitude = fabsf(percent);
    const SteeringMapPoint* map = right ? kRightMap : kLeftMap;
    const size_t count = right ? sizeof(kRightMap) / sizeof(kRightMap[0])
                               : sizeof(kLeftMap) / sizeof(kLeftMap[0]);
    float raw_magnitude = 0.0f;
    if (!interpolate(map, count, magnitude, &raw_magnitude))
    {
        return false;
    }
    *raw_deg = right ? raw_magnitude : -raw_magnitude;
    return true;
}

bool steering_map_percent_to_equivalent(const float percent, float* equivalent_deg)
{
    if (equivalent_deg == NULL || !isfinite(percent) ||
        percent < STEERING_PERCENT_MIN || percent > STEERING_PERCENT_MAX)
    {
        return false;
    }
    *equivalent_deg = percent * (STEERING_EQUIVALENT_MAX_DEG / 100.0f);
    return true;
}

bool steering_map_equivalent_to_percent(const float equivalent_deg, float* percent)
{
    if (percent == NULL || !isfinite(equivalent_deg) ||
        equivalent_deg < -STEERING_EQUIVALENT_MAX_DEG ||
        equivalent_deg > STEERING_EQUIVALENT_MAX_DEG)
    {
        return false;
    }
    *percent = equivalent_deg * (100.0f / STEERING_EQUIVALENT_MAX_DEG);
    return true;
}
