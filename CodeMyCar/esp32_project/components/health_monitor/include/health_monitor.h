#pragma once

/**
 * @file health_monitor.h
 * @brief 健康监控（TWDT + 堆监控 + 心跳）
 */

#include <stddef.h>

#include "esp_err.h"

esp_err_t health_monitor_init(void);
esp_err_t health_monitor_start(void);
size_t health_monitor_get_heap_free(void);
size_t health_monitor_get_heap_min(void);
