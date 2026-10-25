#pragma once

/**
 * @file cmcar_events.h
 * @brief CodeMyCar 应用事件定义（基于 esp_event）
 *
 * 替代旧的全局 EventGroup + BIT0-BIT8 方案。
 * 所有跨组件通信通过此事件循环完成。
 */

#include "esp_event.h"

/* 声明事件基（在 cmcar_events.c 中定义） */
ESP_EVENT_DECLARE_BASE(CMCAR_EVENTS);

/* 应用事件 ID（强类型枚举） */
typedef enum {
    /* WiFi 事件 */
    CMCAR_WIFI_CONNECTED = 0,
    CMCAR_WIFI_DISCONNECTED,
    CMCAR_WIFI_GOT_IP,

    /* MQTT 事件 */
    CMCAR_MQTT_CONNECTED,
    CMCAR_MQTT_DISCONNECTED,

    /* UART 事件（帧走 uart_service_register_rx_cb 直回调，不入事件循环） */
    CMCAR_UART_ERROR,

    /* 系统事件 */
    CMCAR_SYSTEM_READY,
    CMCAR_SYSTEM_SHUTDOWN,
    CMCAR_HEALTH_WARNING,

    CMCAR_EVENT_MAX,
} cmcar_event_id_t;

/* 事件数据：CMCAR_WIFI_GOT_IP */
typedef struct {
    uint32_t ip_addr; /* IPv4 地址（网络字节序） */
} cmcar_wifi_ip_event_t;

/* 事件数据：CMCAR_HEALTH_WARNING */
typedef struct {
    uint32_t heap_free;      /* 当前空闲堆字节数 */
    uint32_t heap_min_ever;  /* 历史最低空闲堆 */
} cmcar_health_warning_t;
