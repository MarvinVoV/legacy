#pragma once

/**
 * @file cmcar_errors.h
 * @brief CodeMyCar 错误码定义
 *
 * 基于 ESP-IDF 标准 esp_err_t，扩展项目专用错误码。
 */

#include "esp_err.h"

#define CMCAR_ERR_BASE 0x8000

/* 项目专用错误码 */
#define CMCAR_ERR_UART_FRAME    (CMCAR_ERR_BASE + 1) /* CRC 校验失败 */
#define CMCAR_ERR_UART_OVERFLOW (CMCAR_ERR_BASE + 2) /* RingBuf 溢出 */
#define CMCAR_ERR_MQTT_PUB      (CMCAR_ERR_BASE + 3) /* MQTT 发布失败 */
#define CMCAR_ERR_MQTT_SUB      (CMCAR_ERR_BASE + 4) /* MQTT 订阅失败 */
#define CMCAR_ERR_QUEUE_FULL    (CMCAR_ERR_BASE + 5) /* 队列满 */
#define CMCAR_ERR_ACK_TIMEOUT   (CMCAR_ERR_BASE + 6) /* ACK 超时 */
#define CMCAR_ERR_INVALID_FRAME (CMCAR_ERR_BASE + 7) /* 无效帧格式 */
#define CMCAR_ERR_NOT_READY     (CMCAR_ERR_BASE + 8) /* 组件未就绪 */
