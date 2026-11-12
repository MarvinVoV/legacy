#pragma once
#include <stdint.h>

typedef struct {
    uint32_t previous;
    uint32_t remainder;
    uint64_t ticks;
} CycleClock;

/* Unsigned subtraction tolerates CYCCNT wrap; call at least once per wrap. */
uint64_t cycle_clock_update(CycleClock *clock, uint32_t cycles, uint32_t cyclesPerTick);
