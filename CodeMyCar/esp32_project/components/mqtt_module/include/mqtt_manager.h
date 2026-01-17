#pragma once
#include <stdint.h>

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// MQTT消息结构
typedef struct
{
    char* topic;      // topic name
    uint8_t* payload; // message data
    size_t length;    // message data length
} mqtt_msg_t;

// MQTT配置参数
typedef struct
{
    const char* broker_url; // MQTT broker URL
    const char* client_id;  // MQTT client ID
} mqtt_config_t;

/**
 * @brief Initialize the MQTT client
 */
void mqtt_init();

/**
 * @brief Start the MQTT client
 */
void mqtt_start_connect();
/**
 * @brief Stop the MQTT client
 */
void mqtt_stop_connect(void);

/**
 * @brief Initialize and start the MQTT client
 */
void mqtt_init_and_start(void);

/**
 * @brief Publish a message to the MQTT broker
 * @param topic Topic name
 * @param data Message data
 * @param len Message length
 * @param qos Quality of Service level
 */
esp_err_t mqtt_publish(const char* topic, const char* data, size_t len,
                       int qos);

/**
 * @brief Subscribe to a topic
 * @param topic Topic name
 * @param qos Quality of Service level
 */
esp_err_t mqtt_subscribe(const char* topic, int qos);

/**
 * 监听EVENT_MQTT_CONNECTED事件，调用该方法创建处理任务
 * @brief Task that handles UART messages
 */
void handle_uart_message_task(void* pvParameters);

#ifdef __cplusplus
}
#endif
