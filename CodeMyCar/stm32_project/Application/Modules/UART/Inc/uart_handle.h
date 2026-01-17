/*
 * uart_handle.h
 *
 *  Created on: Feb 4, 2025
 *      Author: marvin
 */

#ifndef INC_UART_HANDLE_H_
#define INC_UART_HANDLE_H_

#include <pkt_protocol.h>
#include <stdint.h>
#include <stdbool.h>

#include "cmsis_os2.h"
#include "log_protocol.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32h7xx_hal.h"

// 定义UART缓冲区大小
#define UART_RX_BUFFER_SIZE (PROTOCOL_MAX_DATA_LEN * 4)
#define UART_TX_BUFFER_SIZE (PROTOCOL_MAX_DATA_LEN * 4)


typedef struct
{
    const uint8_t* data;
    uint16_t len;
} uart_msg_t;


typedef struct
{
    uint8_t rx_buffer[2][UART_RX_BUFFER_SIZE] __ALIGNED(32);
    uint8_t tx_buffer[UART_TX_BUFFER_SIZE] __ALIGNED(32);
    volatile uint8_t current;    // 当前接收缓冲区索引
    volatile uint16_t length;    // 有效数据长度
    SemaphoreHandle_t semaphore; // 二值信号量
} uart_dma_buffer_t;

#define QUEUE_ITEM_SIZE sizeof(uart_msg_t*) // 存储数据指针

extern uart_dma_buffer_t uart_dma_buffer;
void Init_UART_DMA(void);


/**
 * @brief 发送构建好的日志协议数据到外设(需要外接esp32)
 * @param log_entry log_entry
 * @return bool
 */
bool UART_Send_Protocol_Log(log_entry_t* log_entry);


#endif /* INC_UART_HANDLE_H_ */
