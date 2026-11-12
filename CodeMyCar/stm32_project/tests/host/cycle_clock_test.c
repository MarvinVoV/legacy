#include "cycle_clock.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    CycleClock c = {.previous=UINT32_MAX-99u};
    assert(cycle_clock_update(&c, 50u, 100u) == 1u);
    assert(c.remainder == 50u);
    assert(cycle_clock_update(&c, 99u, 100u) == 1u);
    assert(cycle_clock_update(&c, 100u, 100u) == 2u);
    /* Carry + near-full-wrap delta used to overflow a naive 32-bit sum. */
    c = (CycleClock){.remainder=49999u};
    assert(cycle_clock_update(&c, UINT32_MAX, 50000u) == 85900u);
    assert(c.remainder == 17294u);
    /* Long-term runtime counters retain precision beyond 32-bit ticks. */
    c = (CycleClock){.ticks=UINT32_MAX};
    assert(cycle_clock_update(&c, 100u, 100u) == (uint64_t)UINT32_MAX+1u);
    assert(cycle_clock_update(NULL, 10u, 100u) == 0u);
    assert(cycle_clock_update(&c, 10u, 0u) == 0u);
    for (uint32_t mhz=250u; mhz<=500u; mhz+=250u) {
        c=(CycleClock){0};
        uint32_t cycle=0u;
        for (unsigned i=0u; i<100000u; ++i) {
            cycle+=mhz*1000u;
            cycle_clock_update(&c, cycle, mhz*100u);
        }
        assert(c.ticks == 1000000u); /* 100s at 10kHz, many DWT wraps. */
    }
    puts("cycle clock: wrap, carry, long runtime and 250/500MHz PASS");
}
