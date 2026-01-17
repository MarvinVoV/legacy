#include "queue_manager.h"

#include "esp_log.h"

static const char* TAG = "QueueManager";

// 队列配置参数
#define MQTT_QUEUE_LENGTH 10
#define UART_QUEUE_LENGTH 10
#define QUEUE_ITEM_SIZE sizeof(QueueMessage_t)

static QueueHandle_t mqtt_data_queue = NULL;
static QueueHandle_t uart_data_queue = NULL;
static QueueHandle_t log_data_queue = NULL;

void queue_manager_full_init()
{
    for (QueueType_t type = UART; type < QUEUE_TYPE_MAX; type++)
    {
        if (!queue_manager_init(type))
        {
            ESP_LOGE(TAG, "Failed to create %d queue", type);
        }
    }
}

bool queue_manager_init(const QueueType_t queue_type)
{
    bool res = true;
    switch (queue_type)
    {
        case MQTT:
            if (mqtt_data_queue != NULL)
            {
                return mqtt_data_queue;
            }
        // 创建MQTT发送队列
            mqtt_data_queue = xQueueCreate(MQTT_QUEUE_LENGTH, QUEUE_ITEM_SIZE);
            if (!mqtt_data_queue)
            {
                ESP_LOGE(TAG, "Failed to create MQTT send queue");
                res = false;
            }
            break;
        case UART:
            if (uart_data_queue != NULL)
            {
                return uart_data_queue;
            }
        // 创建UART发送队列
            uart_data_queue = xQueueCreate(UART_QUEUE_LENGTH, QUEUE_ITEM_SIZE);
            if (!uart_data_queue)
            {
                ESP_LOGE(TAG, "Failed to create UART send queue");
                res = false;
            }
            break;
        case LOG:
            if (log_data_queue != NULL)
            {
                return log_data_queue;
            }
        // 创建日志发送队列
            log_data_queue = xQueueCreate(UART_QUEUE_LENGTH, QUEUE_ITEM_SIZE);
            if (!log_data_queue)
            {
                ESP_LOGE(TAG, "Failed to create LOG send queue");
                res = false;
            }
            break;
        default:
            ESP_LOGW(TAG, "Invalid queue type requested: %d", queue_type);
            res = false;
            break;
    }
    return res;
}

QueueHandle_t queue_manager_get_queue(QueueType_t queue_type)
{
    switch (queue_type)
    {
        case MQTT:
            return mqtt_data_queue;
        case UART:
            return uart_data_queue;
        case LOG:
            return log_data_queue;
        default:
            ESP_LOGW(TAG, "Invalid queue type requested: %d", queue_type);
            return NULL;
    }
}

void queue_manager_destroy(QueueType_t queue_type)
{
    switch (queue_type)
    {
        case MQTT:
            vQueueDelete(mqtt_data_queue);
            break;
        case UART:
            vQueueDelete(uart_data_queue);
            break;
        case LOG:
            vQueueDelete(log_data_queue);
            break;
        default:
            ESP_LOGW(TAG, "Invalid queue type requested: %d", queue_type);
            break;
    }
}

void free_queue_message(QueueMessage_t* msg)
{
    if (!msg)
    {
        return;
    }
    if (msg->data)
    {
        free(msg->data);
        msg->data = NULL;
    }
    if (msg->extra_info)
    {
        free(msg->extra_info);
        msg->extra_info = NULL;
    }
    msg->len = 0;
}
