#ifndef STEERING_MAP_H
#define STEERING_MAP_H

#include <stdbool.h>

#define STEERING_PERCENT_MIN (-100.0f)
#define STEERING_PERCENT_MAX (100.0f)
#define STEERING_EQUIVALENT_MAX_DEG (25.0f)

/** Map signed user steering percentage to signed raw servo offset degrees.
 *  Negative is left, positive is right. */
bool steering_map_percent_to_raw(float percent, float* raw_deg);

/** Convert signed percentage to the nominal bicycle-model steering angle. */
bool steering_map_percent_to_equivalent(float percent, float* equivalent_deg);

/** Convert a nominal bicycle-model steering angle to signed percentage. */
bool steering_map_equivalent_to_percent(float equivalent_deg, float* percent);

#endif
