#pragma once

/**
 * @file bridge.h
 * @brief UART↔MQTT 桥接编排层
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/** STM32 存活判定窗口：最近一次有效 UART 帧距今小于该值即视为在线 */
#define BRIDGE_STM32_TIMEOUT_MS 60000

esp_err_t bridge_init(void);
esp_err_t bridge_start(void);
esp_err_t bridge_stop(void);

/** STM32 是否在线（最近 BRIDGE_STM32_TIMEOUT_MS 内收到过有效 UART 帧）。
 *  供 health_monitor 写入心跳的 stm32_connected 字段。 */
bool bridge_stm32_connected(void);

/** 把桥接运行指标格式化为 status JSON 写入 out，返回字节数（≤ n-1）。
 *  形如 {"state":"ONLINE","up_drop":N,"dn_drop":N,"ack_lost":N,"pub_fail":N}
 *  供 health_monitor 周期发往 CMCAR_TOPIC_STATUS。失败返回 0。 */
int bridge_get_status(char* out, size_t n);
