#pragma once
#include <stdint.h>
static inline uint32_t control_critical_enter(void) { return 0; }
static inline void control_critical_exit(uint32_t key) { (void)key; }
