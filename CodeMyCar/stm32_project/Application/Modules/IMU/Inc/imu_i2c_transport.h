#pragma once
#include "stm32h7xx_hal.h"
#include <stdbool.h>

/* I2C1 is owned exclusively by ImuTask. No DMA: stack buffers remain valid
 * until completion, including synchronous quiescence after a timeout. */
HAL_StatusTypeDef ImuI2c_Transfer(bool receive, uint16_t address,
                                uint8_t *buffer, uint16_t size, uint32_t timeoutMs);

/* Written only by ImuTask/its I2C IRQ; snapshots capture the fault BEFORE
 * peripheral recovery. Pin bits: bit0=SCL/PB6, bit1=SDA/PB7. */
extern volatile uint32_t g_imuI2cCompletions, g_imuI2cTimeouts, g_imuI2cErrors;
extern volatile uint32_t g_imuI2cBusy, g_imuI2cRecoveries, g_imuI2cRecoveryFailures;
extern volatile uint32_t g_imuI2cLastIsr, g_imuI2cLastState, g_imuI2cLastError;
extern volatile uint32_t g_imuI2cLastPins;
