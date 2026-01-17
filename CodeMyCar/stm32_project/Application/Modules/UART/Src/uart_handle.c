/*
 * uart_handle.c
 *
 *  Created on: Feb 4, 2025
 *      Author: marvin
 */
#include "uart_handle.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cmsis_os2.h"
#include "log_manager.h"
#include "queue.h"
#include "queue_manager.h"
#include "stm32h7xx_hal_uart.h"

#include "uart_config.h"


#define RAW_DATA_QUEUE_SIZE (16)  // 定义队列大小

uart_dma_buffer_t uart_dma_buffer;


/**
 * 发送日志到 ESP (需要外接 ESP32)
 * @param data log data
 * @param length length
 * @param type protocol_type
 * @return bool
 */
static bool UART_Send_To_Esp(const uint8_t* data, uint16_t length, protocol_type_t type);

void start_reception()
{
    // 先切换缓冲区
    const uint8_t next_buffer = uart_dma_buffer.current ^ 1;
    uart_dma_buffer.current = next_buffer;

    // 启动DMA接收，交替使用缓冲区
    HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_DMA(&huart4, uart_dma_buffer.rx_buffer[next_buffer],
                                                            UART_RX_BUFFER_SIZE);
    if (status != HAL_OK)
    {
        char err_msg[64];
        sprintf(err_msg, "HAL_UARTEx_ReceiveToIdle_DMA Error");
        DEBUG_CONSOLE_OUTPUT(&huart1, "UART Error: %s", err_msg);
    }
}

void Init_UART_DMA(void)
{
    uart_dma_buffer.current = 0;
    uart_dma_buffer.length = 0;
    uart_dma_buffer.semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(uart_dma_buffer.semaphore); // 初始释放

    __HAL_UART_ENABLE_IT(&huart4, UART_IT_IDLE | UART_IT_PE | UART_IT_ERR);
    start_reception();
}


//
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
    if (huart == &huart4)
    {
        // 获取DMA剩余计数
        const uint16_t remaining = __HAL_DMA_GET_COUNTER(huart->hdmarx);
        // 计算实际接收数据长度
        const uint16_t actual_len = UART_RX_BUFFER_SIZE - remaining;
        uart_dma_buffer.length = actual_len;
        start_reception(actual_len);
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(uart_dma_buffer.semaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
//
// void send_hex_via_uart( const uint8_t *data, uint16_t len) {
//     char buffer[256];  // 缓冲区大小需足够容纳转换后的字符串（每字节3字符 + 换行符）
//     uint16_t pos = 0;
//
//     // 转换数据为十六进制字符串
//     for (uint16_t i = 0; i < len && pos < sizeof(buffer); i++) {
//         pos += snprintf(&buffer[pos], sizeof(buffer) - pos, "%02X ", data[i]);
//     }
//
//     LOG_DEBUG_UART(&huart1, "%s\n", buffer);
//     LOG_DEBUG_UART(&huart1, "len=%d\n", len);
// }

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
    if (huart == &huart4)
    {
        const uint32_t err = huart->ErrorCode;
        char err_msg[64] = {0};

        if (err & HAL_UART_ERROR_PE) strcat(err_msg, "ParityError ");
        if (err & HAL_UART_ERROR_NE) strcat(err_msg, "NoiseError ");
        if (err & HAL_UART_ERROR_FE) strcat(err_msg, "FramingError ");
        if (err & HAL_UART_ERROR_ORE) strcat(err_msg, "OverrunError ");
        if (err & HAL_UART_ERROR_DMA) strcat(err_msg, "DMAError ");

        DEBUG_CONSOLE_OUTPUT(&huart1, "UART Error: %s", err_msg);
        // 清除错误标志
        __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_PEF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_OREF);
        // 重置错误码
        huart->ErrorCode = HAL_UART_ERROR_NONE;
    }
}


bool UART_Send_Protocol_Log(log_entry_t* log_entry)
{
    if (log_entry == NULL)
    {
        return false;
    }
    // 计算实际总长度
    const size_t total_size = sizeof(log_entry_t) + strlen(log_entry->message) + 1;

    return UART_Send_To_Esp((uint8_t*)log_entry, total_size, PROTOCOL_TYPE_LOG);
}

static bool UART_Send_To_Esp(const uint8_t* data, const uint16_t length, const protocol_type_t type)
{
    // 打包协议帧
    uint16_t frame_len = 0;
    uint8_t* packed_frame = protocol_pack_frame(type, data, length, &frame_len);

    if (packed_frame != NULL && frame_len > 0 && frame_len <= UART_TX_BUFFER_SIZE)
    {
        // 复制到静态缓冲区
        memcpy(uart_dma_buffer.tx_buffer, packed_frame, frame_len);
        free(packed_frame);  // 立即释放动态内存

        const HAL_StatusTypeDef status =
            HAL_UART_Transmit_DMA(&huart4, uart_dma_buffer.tx_buffer, frame_len);
        return status == HAL_OK;
    }
    if (packed_frame != NULL)
    {
        free(packed_frame);
    }
    return false;
}
