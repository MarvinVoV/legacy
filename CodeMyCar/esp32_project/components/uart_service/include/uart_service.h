#pragma once

/**
 * @file uart_service.h
 * @brief UART 硬件服务（2 任务 pipeline，静态分配，零 malloc）
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pkt_codec.h"

/** 帧接收回调（在 uart_rx 任务上下文中调用） */
typedef void (*uart_frame_rx_cb_t)(const pkt_frame_t* frame, void* user_ctx);

/** 初始化 UART 驱动 + 创建静态资源（不启动任务） */
esp_err_t uart_service_init(void);

/** 启动 RX 和 TX 任务 */
esp_err_t uart_service_start(void);

/** 注册帧接收回调 */
esp_err_t uart_service_register_rx_cb(uart_frame_rx_cb_t cb, void* user_ctx);

/** 发送原始字节到 UART TX（编码后的帧数据） */
esp_err_t uart_service_send(const uint8_t* data, size_t len);

/** Safety-critical send placed at the front of the TX queue. */
esp_err_t uart_service_send_priority(const uint8_t* data, size_t len);

/** Epoch captured with a retry batch. STOP invalidates all older epochs. */
uint32_t uart_service_tx_epoch(void);
esp_err_t uart_service_send_at_epoch(const uint8_t* data, size_t len, uint32_t epoch);
/** Discard older queued/popped TX messages and enqueue STOP as a barrier. */
esp_err_t uart_service_send_stop(const uint8_t* data, size_t len);

/** 停止并释放资源 */
esp_err_t uart_service_stop(void);
