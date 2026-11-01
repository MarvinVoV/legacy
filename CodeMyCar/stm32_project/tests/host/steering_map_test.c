#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "steering_map.h"

static void near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.001f);
}

int main(void)
{
    float out = 0.0f;
    assert(steering_map_percent_to_raw(0.0f, &out)); near(out, 0.0f);
    assert(steering_map_percent_to_raw(-25.0f, &out)); near(out, -15.0f);
    assert(steering_map_percent_to_raw(-50.0f, &out)); near(out, -30.0f);
    assert(steering_map_percent_to_raw(-75.0f, &out)); near(out, -45.0f);
    assert(steering_map_percent_to_raw(-100.0f, &out)); near(out, -60.0f);
    assert(steering_map_percent_to_raw(25.0f, &out)); near(out, 10.0f);
    assert(steering_map_percent_to_raw(50.0f, &out)); near(out, 15.0f);
    assert(steering_map_percent_to_raw(75.0f, &out)); near(out, 24.0f);
    assert(steering_map_percent_to_raw(100.0f, &out)); near(out, 32.0f);
    assert(steering_map_percent_to_raw(62.5f, &out)); near(out, 19.5f);
    assert(!steering_map_percent_to_raw(100.1f, &out));
    assert(!steering_map_percent_to_raw(NAN, &out));
    assert(!steering_map_percent_to_raw(0.0f, NULL));

    assert(steering_map_percent_to_equivalent(100.0f, &out)); near(out, 25.0f);
    assert(steering_map_percent_to_equivalent(-50.0f, &out)); near(out, -12.5f);
    assert(steering_map_equivalent_to_percent(18.75f, &out)); near(out, 75.0f);
    assert(!steering_map_equivalent_to_percent(25.1f, &out));

    puts("steering_map: all tests passed");
    return 0;
}
