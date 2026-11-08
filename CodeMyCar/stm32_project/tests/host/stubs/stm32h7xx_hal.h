#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t counter;
    uint32_t compare;
    struct { uint32_t ARR; } registers;
    struct { uint32_t ARR; } *Instance;
    struct { uint32_t Period; } Init;
} TIM_HandleTypeDef;

typedef struct
{
    uint32_t unused;
    uint32_t pins;
} GPIO_TypeDef;

typedef struct { uint32_t unused; } I2C_HandleTypeDef;
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __disable_irq(void) {}
static inline void __DMB(void) {}
static inline void __set_PRIMASK(uint32_t key) { (void)key; }
#define GPIO_PIN_SET 1u
#define GPIO_PIN_RESET 0u
#define TIM_CHANNEL_ALL 0u
#define __HAL_TIM_SET_COMPARE(t, ch, v) ((void)(ch), (t)->compare = (v))
#define __HAL_TIM_GET_AUTORELOAD(t) ((t)->Init.Period)
#define __HAL_TIM_SET_COUNTER(t, v) ((t)->counter = (v))
#define __HAL_TIM_GET_COUNTER(t) ((t)->counter)
static inline void HAL_GPIO_WritePin(GPIO_TypeDef* p, uint16_t pin, uint32_t v)
{ if (v) p->pins |= pin; else p->pins &= ~pin; }

typedef enum
{
    HAL_OK = 0,
    HAL_ERROR = 1,
} HAL_StatusTypeDef;

uint32_t HAL_GetTick(void);
static inline HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* t, uint32_t c)
{ (void)t; (void)c; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_TIM_Encoder_Start(TIM_HandleTypeDef* t, uint32_t c)
{ (void)t; (void)c; return HAL_OK; }
