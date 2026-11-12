#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *Instance;
    struct { uint32_t Timing, AddressingMode, DualAddressMode, OwnAddress1,
        OwnAddress2, OwnAddress2Masks, GeneralCallMode, NoStretchMode; } Init;
} I2C_HandleTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
typedef int HAL_StatusTypeDef;
#define HAL_OK 0
#define HAL_ERROR 1
#define I2C2 ((void *)2)
#define GPIOB ((void *)3)
#define GPIO_PIN_10 (1u<<10)
#define GPIO_PIN_11 (1u<<11)
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
#define GPIO_MODE_OUTPUT_OD 1
#define GPIO_PULLUP 1
#define GPIO_SPEED_FREQ_LOW 0
#define I2C_ADDRESSINGMODE_7BIT 0
#define I2C_DUALADDRESS_DISABLE 0
#define I2C_OA2_NOMASK 0
#define I2C_GENERALCALL_DISABLE 0
#define I2C_NOSTRETCH_DISABLE 0
#define I2C_ANALOGFILTER_ENABLE 0
#define I2C_MEMADD_SIZE_8BIT 1
#define RCC_I2C123CLKSOURCE_D2PCLK1 0
extern unsigned fakeMux;
#define __HAL_RCC_GET_I2C2_SOURCE() fakeMux
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
typedef struct { const char *name; unsigned stack_size; int priority; } osThreadAttr_t;
#define osPriorityBelowNormal 1
#define PROTOCOL_TYPE_SENSOR 2
#define LOG_MODULE_SENSOR 1
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)

uint32_t HAL_RCC_GetPCLK1Freq(void);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *bus);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *bus);
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *bus,int filter);
HAL_StatusTypeDef HAL_I2C_Mem_Read(void *,uint16_t,uint16_t,uint16_t,uint8_t *,uint16_t,uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Write(void *,uint16_t,uint16_t,uint16_t,uint8_t *,uint16_t,uint32_t);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *,uint16_t,uint32_t,uint32_t);
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *);
void HAL_GPIO_Init(void *,GPIO_InitTypeDef *);
void HAL_GPIO_WritePin(void *,uint32_t,int);
int HAL_GPIO_ReadPin(void *,uint32_t);
uint32_t HAL_GetTick(void);
uint32_t osKernelGetTickCount(void);
void osDelay(uint32_t);
void osDelayUntil(uint32_t);
void *osThreadNew(void (*)(void *),void *,const osThreadAttr_t *);
int UART_Send_Frame(uint8_t,uint8_t,uint8_t,const uint8_t *,uint16_t);
