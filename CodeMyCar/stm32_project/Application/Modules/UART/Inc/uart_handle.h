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
#include "stream_buffer.h"
#include "stm32h7xx_hal.h"

// 定义UART缓冲区大小
#define UART_RX_BUFFER_SIZE (PROTOCOL_MAX_DATA_LEN * 4)
#define UART_TX_BUFFER_SIZE (PROTOCOL_MAX_DATA_LEN * 4)
#define UART_RX_STREAM_SIZE (UART_RX_BUFFER_SIZE * 4)


typedef struct
{
    const uint8_t* data;
    uint16_t len;
} uart_msg_t;


typedef struct
{
    uint8_t rx_buffer[2][UART_RX_BUFFER_SIZE] __ALIGNED(32);
    uint8_t tx_buffer[UART_TX_BUFFER_SIZE] __ALIGNED(32);
    volatile uint8_t current; // 当前 DMA 正在写入的缓冲区索引
} uart_dma_buffer_t;

#define QUEUE_ITEM_SIZE sizeof(uart_msg_t*) // 存储数据指针

extern uart_dma_buffer_t uart_dma_buffer;
void Init_UART_DMA(void);

/** 从 ISR 填充的静态 RX stream buffer 读取数据。 */
size_t UART_Receive(uint8_t* out, size_t capacity, uint32_t timeout_ms);

/** 因 RX stream buffer 空间不足而丢弃的字节数。 */
uint32_t UART_GetRxDropCount(void);


/**
 * @brief 发送构建好的日志协议数据到外设(需要外接esp32)
 * @param log_entry log_entry
 * @return bool
 */
bool UART_Send_Protocol_Log(log_entry_t* log_entry);

/**
 * @brief 发送 v2 协议帧到 ESP32（支持 seq/flags，用于 ACK 回复等）
 * @param type   协议类型
 * @param seq    序列号
 * @param flags  FLAGS（ACK_REQ/ACK_RSP）
 * @param data   payload（可为 NULL）
 * @param len    payload 长度
 * @return true 成功发送
 */
bool UART_Send_Frame(protocol_type_t type, uint8_t seq, uint8_t flags,
                     const uint8_t* data, uint16_t len);


#endif /* INC_UART_HANDLE_H_ */
