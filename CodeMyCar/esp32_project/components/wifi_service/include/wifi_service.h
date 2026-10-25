#pragma once

/**
 * @file wifi_service.h
 * @brief WiFi STA 服务（事件驱动，指数退避重连）
 */

#include <stdbool.h>

#include "esp_err.h"

/** 初始化 WiFi（netif + 驱动 + 事件注册，不连接） */
esp_err_t wifi_service_init(void);

/** 启动 WiFi 连接 */
esp_err_t wifi_service_start(void);

/** 停止 WiFi */
esp_err_t wifi_service_stop(void);

/** 查询连接状态 */
bool wifi_service_is_connected(void);
