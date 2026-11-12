#pragma once
#include <stdint.h>

void RuntimeStats_Start(void);
uint64_t RuntimeStats_Counter(void);
void RuntimeStats_TaskInit(void);

/* No allocation/logging/RTOS locks: stop PWM before allowing watchdog recovery. */
void RuntimeStats_Fatal(uint32_t reason);
