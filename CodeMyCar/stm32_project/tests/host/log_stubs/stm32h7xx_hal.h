#pragma once
#include "../runtime_stubs/stm32h7xx_hal.h"
typedef struct { int id; } UART_HandleTypeDef;
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef*,uint8_t*,uint16_t,uint32_t);
