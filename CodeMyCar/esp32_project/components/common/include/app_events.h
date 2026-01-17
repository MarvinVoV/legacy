#pragma once
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// 系统级事件位定义
typedef enum
{
    EVENT_WIFI_CONNECTED = BIT0,    // WiFi连接成功
    EVENT_WIFI_DISCONNECTED = BIT1, // WiFi断开连接
    EVENT_WIFI_GOT_IP = BIT2,       // WiFi已获取IP地址

    EVENT_HTTP_REQ_SENT = BIT3, // HTTP请求已发送
    EVENT_HTTP_RSP_RCVD = BIT4, // HTTP响应已接收

    EVENT_MQTT_CONNECTED = BIT5,    // MQTT连接成功
    EVENT_MQTT_DISCONNECTED = BIT6, // MQTT连接断开

    UART_INIT_COMPLETE = BIT7, // UART初始化完成
    UART_ERROR = BIT8,         // UART错误
} SystemEventBits;

/**
 * 初始化应用事件组
 */
void app_events_init(void);
/**
 * 获取应用事件组句柄
 * @return 应用事件组句柄
 */
EventGroupHandle_t app_events_get_group(void);

/**
 * 设置事件组中的事件位
 * @param bits 要设置的事件位
 */
void app_events_clear_bits(EventBits_t bits);
/**
 * 获取事件组中的事件位
 * @param bits 要获取的事件位
 * @return 事件位
 */
void app_events_set_bits(EventBits_t bits);

/**
 * 获取当前事件组中的事件位
 * @return 当前事件组中的事件位
 */
EventBits_t app_events_get_bits(void);

/**
 * 原子地清除某些事件位并设置另一些事件位
 * @param bits_to_clear 要清除的事件位
 * @param bits_to_set 要设置的事件位
 */
void app_events_set_bits_atomic(EventBits_t bits_to_clear,
                                EventBits_t bits_to_set);

#ifdef __cplusplus
}
#endif
