#pragma once
#include "stm32h7xx_hal.h"
#include <stdbool.h>

/* I2C1 is owned exclusively by ImuTask. No DMA: stack buffers remain valid
 * until completion, including synchronous quiescence after a timeout. */
HAL_StatusTypeDef ImuI2c_Transfer(bool receive, uint16_t address,
                                uint8_t *buffer, uint16_t size, uint32_t timeoutMs);
