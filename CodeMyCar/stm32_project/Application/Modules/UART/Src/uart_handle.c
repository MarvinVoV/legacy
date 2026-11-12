/*
 * uart_handle.c
 *
 *  Created on: Feb 4, 2025
 *      Author: marvin
 */
#include "uart_handle.h"

#include <string.h>
#include <stdio.h>

#include "cmsis_os2.h"
#include "log_manager.h"
#include "queue.h"
#include "queue_manager.h"
#include "stm32h7xx_hal_uart.h"

#include "uart_config.h"


#define RAW_DATA_QUEUE_SIZE (16)  // 定义队列大小

/* DMA1/DMA2 不能访问 DTCM（0x2000_0000）；RX/TX 缓冲区必须放在 DMA 可达的
   RAM_D1（0x2400_0000）。链接脚本中 .dma_buffer 段位于 RAM_D1。 */
uart_dma_buffer_t uart_dma_buffer __attribute__((section(".dma_buffer"), aligned(32)));

/* TX 串行化：ACK / 日志 / 运动状态共用同一个 DMA 缓冲区 */
static StaticSemaphore_t s_tx_mutex_storage;
static SemaphoreHandle_t s_tx_mutex;

/* ISR 是唯一生产者，CtrlTask 是唯一消费者。ISR 在重新启用 DMA 前先把
   完成块复制进 stream buffer，因此两个 DMA buffer 都不会被任务侧引用。 */
static StaticStreamBuffer_t s_rx_stream_struct;
static uint8_t s_rx_stream_storage[UART_RX_STREAM_SIZE];
static StreamBufferHandle_t s_rx_stream;
static volatile uint32_t s_rx_dropped_bytes;
static volatile uint32_t s_uart_errors;
static volatile uint32_t s_rx_start_failures;

/* All UART DMA memory is in MPU non-cacheable DMA_RAM. Barriers publish
 * CPU writes before DMA starts and order DMA completion before CPU reads. */
static void dma_rx_prepare(uint8_t* buffer)
{
    (void)buffer;
    __DMB();
}

static void dma_rx_complete(uint8_t* buffer, uint16_t length)
{
    (void)buffer;
    (void)length;
    __DMB();
}

static void dma_tx_prepare(uint8_t* buffer, uint16_t length)
{
    (void)buffer;
    (void)length;
    __DMB();
}


/**
 * 发送日志到 ESP (需要外接 ESP32)
 * @param data log data
 * @param length length
 * @param type protocol_type
 * @return bool
 */
static bool UART_Send_To_Esp(const uint8_t* data, uint16_t length, protocol_type_t type);

static void start_reception(uint8_t buffer_index)
{
    uart_dma_buffer.current = buffer_index;
    dma_rx_prepare(uart_dma_buffer.rx_buffer[buffer_index]);
    HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_DMA(&huart4, uart_dma_buffer.rx_buffer[buffer_index],
                                                            UART_RX_BUFFER_SIZE);
    if (status == HAL_OK)
    {
        __HAL_DMA_DISABLE_IT(huart4.hdmarx, DMA_IT_HT);
    }
    if (status != HAL_OK)
    {
        ++s_rx_start_failures;
    }
}

void Init_UART_DMA(void)
{
    uart_dma_buffer.current = 0;
    s_rx_dropped_bytes = 0;
    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_storage);
    s_rx_stream = xStreamBufferCreateStatic(UART_RX_STREAM_SIZE, 1u,
                                             s_rx_stream_storage,
                                             &s_rx_stream_struct);

    __HAL_UART_ENABLE_IT(&huart4, UART_IT_IDLE | UART_IT_PE | UART_IT_ERR);
    start_reception(0u);
}


//
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
    if (huart == &huart4)
    {
        const uint8_t completed = uart_dma_buffer.current;
        const uint16_t actual_len = Size <= UART_RX_BUFFER_SIZE
                                        ? Size : UART_RX_BUFFER_SIZE;
        uint8_t* const data = uart_dma_buffer.rx_buffer[completed];
        dma_rx_complete(data, actual_len);

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        const size_t sent = xStreamBufferSendFromISR(
            s_rx_stream, data, actual_len, &xHigherPriorityTaskWoken);
        if (sent < actual_len)
        {
            s_rx_dropped_bytes += (uint32_t)(actual_len - sent);
        }

        start_reception(completed ^ 1u);
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
        ++s_uart_errors;
        // 清除错误标志
        __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_PEF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_OREF);
        // 重置错误码
        huart->ErrorCode = HAL_UART_ERROR_NONE;

        /* 错误会使 HAL 终止接收（abort DMA、RxState=READY）。
           必须重新挂起 DMA，否则一次 ORE 之后 RX 永久停止。 */
        start_reception(uart_dma_buffer.current ^ 1u);
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

bool UART_Send_Frame(const protocol_type_t type, const uint8_t seq, const uint8_t flags,
                     const uint8_t* data, const uint16_t len)
{
    if ((data == NULL && len > 0u) || len > PROTOCOL_MAX_PAYLOAD ||
        s_tx_mutex == NULL || xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(100u)) != pdTRUE)
    {
        return false;
    }

    const uint32_t wait_start = HAL_GetTick();
    while (huart4.gState != HAL_UART_STATE_READY &&
           (HAL_GetTick() - wait_start) < 20u)
    {
        osDelay(1);
    }
    if (huart4.gState != HAL_UART_STATE_READY)
    {
        xSemaphoreGive(s_tx_mutex);
        return false;
    }

    protocol_frame_t frame = {
        .type = (uint8_t)type,
        .seq = seq,
        .flags = flags,
        .payload_len = (uint8_t)len,
    };
    if (len > 0u)
    {
        memcpy(frame.payload, data, len);
    }

    bool ok = false;
    size_t frame_len = 0;
    if (protocol_encode(&frame, uart_dma_buffer.tx_buffer,
                        UART_TX_BUFFER_SIZE, &frame_len) == 0 && frame_len > 0u)
    {
        dma_tx_prepare(uart_dma_buffer.tx_buffer, (uint16_t)frame_len);
        ok = (HAL_UART_Transmit_DMA(&huart4, uart_dma_buffer.tx_buffer,
                                    (uint16_t)frame_len) == HAL_OK);
    }

    xSemaphoreGive(s_tx_mutex);
    return ok;
}

size_t UART_Receive(uint8_t* out, size_t capacity, uint32_t timeout_ms)
{
    if (out == NULL || capacity == 0u || s_rx_stream == NULL)
    {
        return 0u;
    }
    return xStreamBufferReceive(s_rx_stream, out, capacity,
                                pdMS_TO_TICKS(timeout_ms));
}

uint32_t UART_GetRxDropCount(void)
{
    return s_rx_dropped_bytes;
}

static bool UART_Send_To_Esp(const uint8_t* data, const uint16_t length, const protocol_type_t type)
{
    // seq=0, flags=0 的普通帧（兼容旧 TX 路径）
    return UART_Send_Frame(type, 0, 0, data, length);
}

uint32_t UART_GetErrorCount(void) { return s_uart_errors; }
uint32_t UART_GetRxStartFailureCount(void) { return s_rx_start_failures; }
