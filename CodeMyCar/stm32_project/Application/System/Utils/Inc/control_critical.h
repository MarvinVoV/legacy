#pragma once

#include "stm32h7xx_hal.h"

/* Only short memory copies / actuator register writes belong in this section.
 * Never acquire an RTOS mutex, log, or perform I2C/UART here. */
static inline uint32_t control_critical_enter(void)
{
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return saved;
}

static inline void control_critical_exit(uint32_t saved)
{
    __DMB();
    __set_PRIMASK(saved);
}
