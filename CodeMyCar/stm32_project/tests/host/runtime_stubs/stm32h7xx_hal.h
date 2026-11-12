#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { HAL_OK=0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint8_t *pBuffPtr; } I2C_HandleTypeDef;
enum { I2C1_EV_IRQn=1, I2C1_ER_IRQn=2 };
HAL_StatusTypeDef HAL_I2C_Master_Receive_IT(I2C_HandleTypeDef*,uint16_t,uint8_t*,uint16_t);
HAL_StatusTypeDef HAL_I2C_Master_Transmit_IT(I2C_HandleTypeDef*,uint16_t,uint8_t*,uint16_t);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef*);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef*);
void HAL_I2C_EV_IRQHandler(I2C_HandleTypeDef*);
void HAL_I2C_ER_IRQHandler(I2C_HandleTypeDef*);
void HAL_NVIC_SetPriority(int,unsigned,unsigned);
void HAL_NVIC_ClearPendingIRQ(int);
void HAL_NVIC_EnableIRQ(int);
void HAL_NVIC_DisableIRQ(int);
