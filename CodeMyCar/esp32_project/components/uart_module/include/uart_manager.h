#pragma once
#include <stdint.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
// TX RX 缓冲区大小
#define UART_BUF_SIZE (512)
// 环形缓冲区大小
#define RING_BUFFER_SIZE (UART_BUF_SIZE * 2)

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief 初始化UART模块
 */
void uart_init();

/**
 * @brief 向UART发送封装协议帧数据
 * @param data raw data
 * @param len raw data length
 * @return 是否发送成功
 */
bool uart_send_data(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
