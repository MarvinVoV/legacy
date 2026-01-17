#include "mqtt_manager.h"

#include "app_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mqtt_topic.h"
#include "mqtt_utils.h"
#include "queue_manager.h"
#include "task_priorities.h"
#include "pkt_protocol_buf.h"

#define TAG "MQTT"
#define MAX_CONN_RETRIES 5
#define MAX_SUBSCRIBE_RETRIES 3

static int s_retry_count = 0;

// 定义MQTT配置
static mqtt_config_t* mqtt_config = NULL;

// 定义全局MQTT客户端句柄
static esp_mqtt_client_handle_t mqtt_client;

// 定义MQTT连接尝试次数
static bool s_mqtt_task_running = false;

// 函数声明
static void process_mqtt_message(const mqtt_msg_t* msg);
static void handle_command_message(const mqtt_msg_t* msg);
static void handle_common_message(const mqtt_msg_t* msg);
esp_err_t mqtt_subscribe_with_retry(const char* topic, int qos);

/**
 * @brief MQTT事件处理函数
 * @param handler_args 事件处理函数参数
 * @param base 事件基础
 * @param event_id 事件ID
 * @param event_data 事件数据
 */
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    ESP_LOGD(TAG,
             "Event dispatched from event loop base=%s, event_id=%" PRIi32 "",
             base, event_id);
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to broker");
            s_retry_count = 0;
            app_events_set_bits(EVENT_MQTT_CONNECTED);
        // 订阅服务端消息
            const esp_err_t err = mqtt_subscribe_with_retry(TOPIC_SERVER_CTRL, 0);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to subscribe to topic: %s", TOPIC_SERVER_CTRL);
            }
            else
            {
                ESP_LOGI(TAG, "Subscribed to topic: %s", TOPIC_SERVER_CTRL);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from broker");
            if (s_retry_count < MAX_CONN_RETRIES)
            {
                s_retry_count++;
                ESP_LOGI(TAG, "Retrying MQTT connection (attempt %d/%d)", s_retry_count,
                         MAX_CONN_RETRIES);
                esp_mqtt_client_reconnect(mqtt_client);
            }
            else
            {
                app_events_set_bits_atomic(EVENT_MQTT_CONNECTED,
                                           EVENT_MQTT_DISCONNECTED);
            }
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received data: topic=%.*s", event->topic_len,
                     event->topic);
            const mqtt_msg_t msg = {
                .topic = strndup(event->topic, event->topic_len),
                .payload = malloc(event->data_len), // 分配内存存储二进制数据
                .length = event->data_len
            };

            if (msg.topic && msg.payload)
            {
                memcpy(msg.payload, event->data, event->data_len);
                // 内存释放职责由接收方负责
                process_mqtt_message(&msg);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to allocate memory for MQTT message");
                free(msg.topic);
                free(msg.payload);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribed to topic, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Unsubscribed from topic, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Published message, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error: %d", event->error_handle->error_type);
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

// MQTT连接任务
static void mqtt_connect_task(void* pvParameters)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker =
        {
            .address.uri = mqtt_config->broker_url,
        },
        .credentials =
        {
            .client_id = mqtt_config->client_id,
        },
        .session = {
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1, // 明确指定协议版本
            .keepalive = 300, // 增加到 300 秒,如果服务器在 1.5 倍 KeepAlive
            // 时间内未收到心跳包，会主动断开连接。
            .disable_keepalive = false, // 启用 KeepAlive
        },
        .buffer =  {
            .size = 2048
        }

    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vTaskDelete(NULL);
        return;
    }

    // 注册事件处理函数
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    // 启动MQTT客户端
    esp_mqtt_client_start(mqtt_client);

    // 删除任务
    vTaskDelete(NULL);
}

void mqtt_init()
{
    const char* broker_url = CONFIG_MQTT_BROKER_URL;
    const char* client_id = CONFIG_MQTT_CLIENT_ID;
    mqtt_config = (mqtt_config_t*)malloc(sizeof(mqtt_config_t));
    if (mqtt_config == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for MQTT configuration");
        return;
    }
    mqtt_config->broker_url = strdup(broker_url);
    mqtt_config->client_id = strdup(client_id);
    if (!mqtt_config->broker_url || !mqtt_config->client_id)
    {
        ESP_LOGE(TAG, "MQTT broker URL or client ID not configured");
        free((void*)mqtt_config->broker_url);
        free((void*)mqtt_config->client_id);
        free(mqtt_config);
        mqtt_config = NULL;
        return;
    }

    // 创建MQTT消息发送队列
    if (!queue_manager_init(MQTT))
    {
        ESP_LOGE(TAG, "Failed to create MQTT to UART queue");
        free(mqtt_config);
        mqtt_config = NULL;
        return;
    }

    // 注册MQTT事件到全局事件组
    app_events_init();
}

// 开始连接MQTT
void mqtt_start_connect()
{
    if (!s_mqtt_task_running)
    {
        s_mqtt_task_running = true;
        xTaskCreate(mqtt_connect_task, "mqtt_conn", 4096, NULL,
                    TASK_PRIORITY_MQTT_MANAGER, NULL);
    }
    else
    {
        ESP_LOGW(TAG, "MQTT connect task is already running!");
    }
}

void mqtt_stop_connect(void)
{
    if (mqtt_client)
    {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    s_mqtt_task_running = false;
}

void mqtt_init_and_start(void)
{
    mqtt_init();
    mqtt_start_connect();
}

esp_err_t mqtt_publish(const char* topic, const char* data, const size_t len,
                       const int qos)
{
    if (!mqtt_client)
    {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (qos < 0 || qos > 2)
    {
        ESP_LOGE(TAG, "Invalid QoS value: %d", qos);
        return ESP_ERR_INVALID_ARG;
    }
    const int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data, len, qos, 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_subscribe(const char* topic, int qos)
{
    if (!mqtt_client)
    {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (qos < 0 || qos > 2)
    {
        ESP_LOGE(TAG, "Invalid QoS value: %d", qos);
        return ESP_ERR_INVALID_ARG;
    }
    const int msg_id = esp_mqtt_client_subscribe(mqtt_client, topic, qos);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

static void process_mqtt_message(const mqtt_msg_t* msg)
{
    // ESP_LOGI(TAG, "Processing MQTT message: %.*s, topic=%.*s", msg->length,
    //          msg->payload, strlen(msg->topic), msg->topic);

    // 处理MQTT消息
    if (msg->topic && msg->payload)
    {
        // 下行 Topic
        if (mqtt_topic_match(TOPIC_SERVER_CTRL, msg->topic))
        {
            // 处理控制消息:发送UART数据
            handle_command_message(msg);
        } else
        {
            // 处理通用消息
            handle_common_message(msg);
        }
    }

    // 释放消息内存
    free(msg->topic);
    free(msg->payload);
}

static void handle_command_message(const mqtt_msg_t* msg)
{
    ESP_LOGD(TAG, "Received MQTT message: topic=%s, payload=%.*s, length=%zu",
             msg->topic, msg->length, msg->payload, msg->length);
    if (msg->length <= 0)
    {
        ESP_LOGE(TAG, "Invalid MQTT message length");
        return;
    }
    QueueHandle_t mqtt_data_queue = queue_manager_get_queue(MQTT);
    if (mqtt_data_queue == NULL)
    {
        ESP_LOGE(TAG, "MQTT Send queue not initialized");
        return;
    }
    // 注意这里需要深度拷贝
    QueueMessage_t msg_to_send = {
        .type = MQTT,
        .data = malloc(msg->length),
        .len = msg->length,
        .extra_info = strdup(msg->topic),
    };
    if (!msg_to_send.data || !msg_to_send.extra_info)
    {
        ESP_LOGE(TAG, "Malloc failed");
        msg_to_send.data = NULL;
        msg_to_send.extra_info = NULL;
        return;
    }
    memcpy(msg_to_send.data, msg->payload, msg->length);

    if (xQueueSend(mqtt_data_queue, &msg_to_send, pdMS_TO_TICKS(10)) != pdPASS)
    {
        ESP_LOGE(TAG,
                 "Failed to send message to MQTT send queue, dropping message");
        free(msg_to_send.data);
        free(msg_to_send.extra_info);
        msg_to_send.data = NULL;
        msg_to_send.extra_info = NULL;
    }
    else
    {
        ESP_LOGD(TAG, "Sent message to MQTT send queue successfully.");
    }
}

static void handle_common_message(const mqtt_msg_t* msg)
{
    // 打印消息内容
    ESP_LOGI(TAG, "Received message: %.*s", msg->length, msg->payload);
}

esp_err_t mqtt_subscribe_with_retry(const char* topic, int qos)
{
    for (int retry = 0; retry < MAX_SUBSCRIBE_RETRIES; retry++)
    {
        const esp_err_t err = mqtt_subscribe(topic, qos);
        if (err == ESP_OK)
        {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Retry subscribing to topic: %s (attempt %d/%d)", topic,
                 retry + 1, MAX_SUBSCRIBE_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒后重试
    }
    return ESP_FAIL;
}

void handle_uart_message_task(void* pvParameters)
{
    QueueHandle_t uart_send_queue = NULL;
    // Wait up to 5 seconds for queue initialization
    const TickType_t max_queue_wait = pdMS_TO_TICKS(5000);
    int retry_count = 0;
    while ((uart_send_queue = queue_manager_get_queue(UART)) == NULL)
    {
        const int max_retries = 5;
        if (retry_count++ >= max_retries)
        {
            ESP_LOGE(TAG, "Failed to initialize UART send queue after %d retries",
                     max_retries);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "UART send queue not available, retrying (%d/%d)",
                 retry_count, max_retries);
        vTaskDelay(max_queue_wait / max_retries);
    }
    ESP_LOGD(TAG, "UART send queue available");

    const TickType_t queue_timeout = pdMS_TO_TICKS(1000); // 1s超时
    while (1)
    {
        QueueMessage_t uart_msg = {0};
        const BaseType_t receive_status =
            xQueueReceive(uart_send_queue, &uart_msg, queue_timeout);
        if (receive_status != pdPASS)
        {
            if (receive_status == errQUEUE_EMPTY)
            {
                ESP_LOGD(TAG, "UART queue empty after 1 second timeout");
                // TODO 可检查系统状态 如低功耗模式等 让出CPU资源
            }
            else
            {
                ESP_LOGE(TAG, "Failed to receive message from UART queue (error: %d)",
                         receive_status);
            }
            continue;
        }

        // validate
        if (uart_msg.data == NULL || uart_msg.len == 0)
        {
            ESP_LOGE(TAG,
                     "Invalid data received from uart_send_queue (null data or zero "
                     "length)");
            free_queue_message(&uart_msg);
            continue;
        }
        ESP_LOGI(TAG, "Received message from UART protocol_type=%d", uart_msg.protocol_type);
        // 上行 TOPIC 根据协议类型，发送不同的 topic
        const uint8_t protocol_type = uart_msg.protocol_type;
        const char* topic = protocol_type == PROTOCOL_TYPE_LOG
                               ? TOPIC_STM32_LOG
                               : TOPIC_STM32_SENSOR;
        const esp_err_t err = mqtt_publish(topic, uart_msg.data, uart_msg.len, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to publish uart data to MQTT");
        }
        else
        {
            ESP_LOGD(TAG, "Uart data published to MQTT successfully");
        }
        free_queue_message(&uart_msg);
    }
}
