#pragma once

/**
 * @file mqtt_service.h
 * @brief MQTT 客户端服务（线程安全，指数退避重连）
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** MQTT 下行消息回调 */
typedef void (*mqtt_rx_cb_t)(const char* topic, size_t topic_len,
                              const uint8_t* payload, size_t payload_len,
                              void* user_ctx);

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);
esp_err_t mqtt_service_destroy(void);
bool mqtt_service_is_connected(void);

/** 线程安全的发布 */
esp_err_t mqtt_service_publish(const char* topic, const void* data,
                                size_t len, int qos);

/** 线程安全的发布（带超时）。拿不到锁/发布失败返回非 ESP_OK。
 *  用于看门狗喂狗任务等不能被 publish 无限阻塞的上下文。 */
esp_err_t mqtt_service_publish_timed(const char* topic, const void* data,
                                     size_t len, int qos, uint32_t timeout_ms);

/** 订阅 topic */
esp_err_t mqtt_service_subscribe(const char* topic, int qos);

/** 注册下行消息回调 */
esp_err_t mqtt_service_register_rx_cb(mqtt_rx_cb_t cb, void* user_ctx);
