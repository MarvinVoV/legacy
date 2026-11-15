#pragma once
#include <stdint.h>
typedef struct { uint32_t DEMCR; } CoreDebug_Type;
typedef struct { uint32_t CTRL,CYCCNT; } DWT_Type;
extern CoreDebug_Type display_test_debug;
extern DWT_Type display_test_dwt;
#define CoreDebug (&display_test_debug)
#define DWT (&display_test_dwt)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
extern uint32_t SystemCoreClock;
typedef struct {
 uint32_t Mode,Direction,DataSize,CLKPolarity,CLKPhase,NSS,BaudRatePrescaler;
 uint32_t FirstBit,TIMode,CRCCalculation,CRCPolynomial,FifoThreshold;
} SPI_InitTypeDef;
typedef struct { void *Instance;SPI_InitTypeDef Init; } SPI_HandleTypeDef;
typedef struct { uint32_t Pin,Mode,Pull,Speed,Alternate; } GPIO_InitTypeDef;
#define SPI2 ((void*)2)
#define GPIOB ((void*)3)
#define GPIO_PIN_9  (1u<<9)
#define GPIO_PIN_12 (1u<<12)
#define GPIO_PIN_13 (1u<<13)
#define GPIO_PIN_14 (1u<<14)
#define GPIO_PIN_15 (1u<<15)
#define GPIO_AF5_SPI2 5u
#define GPIO_MODE_AF_PP 0u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 0u
#define GPIO_MODE_OUTPUT_PP 0u
#define GPIO_SPEED_FREQ_HIGH 0u
#define GPIO_PIN_RESET 0u
#define GPIO_PIN_SET 1u
#define SPI_MODE_MASTER 0u
#define SPI_DIRECTION_2LINES 0u
#define SPI_DATASIZE_8BIT 0u
#define SPI_POLARITY_LOW 0u
#define SPI_PHASE_1EDGE 0u
#define SPI_NSS_SOFT 0u
#define SPI_BAUDRATEPRESCALER_16 16u
#define SPI_FIRSTBIT_MSB 0u
#define SPI_TIMODE_DISABLE 0u
#define SPI_CRCCALCULATION_DISABLE 0u
#define SPI_FIFO_THRESHOLD_01DATA 0u
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI2_CLK_ENABLE() ((void)0)
typedef enum {HAL_OK=0,HAL_ERROR=1} HAL_StatusTypeDef;
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi,uint8_t *data,uint16_t size,uint32_t timeout);
void HAL_GPIO_Init(void *port,GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(void *port,uint32_t pin,uint32_t value);
