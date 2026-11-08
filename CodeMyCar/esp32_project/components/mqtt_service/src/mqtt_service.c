/**
 * @file mqtt_service.c
 * @brief MQTT 客户端服务实现
 *
 * 特性：mutex 保护句柄、ESP-MQTT 异步自动重连、直接引用 Kconfig 字符串（不 strdup）
 */

#include "mqtt_service.h"

#include "cmcar_errors.h"
#include "cmcar_events.h"
#include "cmcar_topics.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char* TAG = "mqtt_svc";

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_connected = false;
static bool s_running = false;

static mqtt_rx_cb_t s_rx_cb = NULL;
static void* s_rx_cb_ctx = NULL;

/* MQTT 事件处理器 */
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            if (!s_running)
            {
                break;
            }
            ESP_LOGI(TAG, "Connected to broker");
            s_connected = true;

            /* 自动订阅本机命令 topic（单一 device_id 层级，见 docs/mqtt-topics.md） */
            /* Critical STOP uses QoS1. QoS0 velocity updates remain QoS0. */
            esp_mqtt_client_subscribe(s_client, CMCAR_TOPIC_CMD_SUB, 1);

            esp_event_post(CMCAR_EVENTS, CMCAR_MQTT_CONNECTED, NULL, 0,
                           pdMS_TO_TICKS(100));
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from broker");
            s_connected = false;
            esp_event_post(CMCAR_EVENTS, CMCAR_MQTT_DISCONNECTED, NULL, 0,
                           pdMS_TO_TICKS(100));

            /* ESP-MQTT 自身的重连状态机负责延时和重试；事件回调不得阻塞。 */
            if (s_running)
            {
                ESP_LOGI(TAG, "Waiting for ESP-MQTT automatic reconnect");
            }
            break;

        case MQTT_EVENT_DATA:
            if (s_rx_cb != NULL)
            {
                s_rx_cb(event->topic, (size_t)event->topic_len,
                        (const uint8_t*)event->data,
                        (size_t)event->data_len, s_rx_cb_ctx);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error: %d", event->error_handle->error_type);
            break;

        default:
            break;
    }
}

esp_err_t mqtt_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_CMCAR_MQTT_BROKER_URL,
        .credentials.client_id = CONFIG_CMCAR_MQTT_CLIENT_ID,
        /* 用户名/密码为空则匿名连接（向后兼容） */
        .credentials.username =
            CONFIG_CMCAR_MQTT_USERNAME[0] ? CONFIG_CMCAR_MQTT_USERNAME : NULL,
        .credentials.authentication.password =
            CONFIG_CMCAR_MQTT_PASSWORD[0] ? CONFIG_CMCAR_MQTT_PASSWORD : NULL,
        .session = {
            .keepalive = CONFIG_CMCAR_MQTT_KEEPALIVE_SEC,
            .disable_keepalive = false,
        },
        .buffer = {
            .size = CONFIG_CMCAR_MQTT_BUFFER_SIZE,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "MQTT service initialized (broker: %s)",
             CONFIG_CMCAR_MQTT_BROKER_URL);
    return ESP_OK;
}

esp_err_t mqtt_service_start(void)
{
    if (s_client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    const esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK)
    {
        s_running = false;
    }
    return err;
}

esp_err_t mqtt_service_stop(void)
{
    if (s_client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = false;
    s_connected = false;
    return esp_mqtt_client_stop(s_client);
}

esp_err_t mqtt_service_destroy(void)
{
    s_running = false;
    if (s_client != NULL)
    {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return ESP_OK;
}

bool mqtt_service_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_service_publish(const char* topic, const void* data,
                                size_t len, int qos)
{
    /* 无超时版本：保持向后兼容，调用方不在乎阻塞时用 */
    return mqtt_service_publish_timed(topic, data, len, qos, UINT32_MAX);
}

esp_err_t mqtt_service_publish_timed(const char* topic, const void* data,
                                     size_t len, int qos, uint32_t timeout_ms)
{
    if (s_client == NULL || topic == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t ticks =
        (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_mutex, ticks) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    const int msg_id =
        esp_mqtt_client_publish(s_client, topic, (const char*)data,
                                (int)len, qos, 0);
    xSemaphoreGive(s_mutex);

    if (msg_id < 0)
    {
        return CMCAR_ERR_MQTT_PUB;
    }
    return ESP_OK;
}

esp_err_t mqtt_service_subscribe(const char* topic, int qos)
{
    if (s_client == NULL || topic == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int msg_id = esp_mqtt_client_subscribe(s_client, topic, qos);
    xSemaphoreGive(s_mutex);

    if (msg_id < 0)
    {
        return CMCAR_ERR_MQTT_SUB;
    }
    return ESP_OK;
}

esp_err_t mqtt_service_register_rx_cb(mqtt_rx_cb_t cb, void* user_ctx)
{
    if (cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_rx_cb = cb;
    s_rx_cb_ctx = user_ctx;
    return ESP_OK;
}
