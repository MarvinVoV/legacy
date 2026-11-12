#include "cycle_clock.h"

uint64_t cycle_clock_update(CycleClock *clock, uint32_t cycles, uint32_t cyclesPerTick)
{
    if (!clock || !cyclesPerTick) return 0;
    const uint32_t delta = cycles - clock->previous;
    clock->previous = cycles;
    /* Split before adding the remainder so a near-UINT32_MAX delta cannot overflow. */
    clock->ticks += delta / cyclesPerTick;
    const uint64_t remainder = (uint64_t)(delta % cyclesPerTick) + clock->remainder;
    clock->ticks += remainder / cyclesPerTick;
    clock->remainder = (uint32_t)(remainder % cyclesPerTick);
    return clock->ticks;
}
