#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t counter;
} TIM_HandleTypeDef;

typedef struct
{
    uint32_t unused;
} GPIO_TypeDef;

typedef enum
{
    HAL_OK = 0,
    HAL_ERROR = 1,
} HAL_StatusTypeDef;

uint32_t HAL_GetTick(void);

