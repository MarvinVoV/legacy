//
// Created by marvin on 2025/2/16.
//

#include "mqtt_logger.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include "queue_manager.h"
#include "mqtt_manager.h"
#include "esp_log.h"
#include "mqtt_topic.h"
#include "task_priorities.h"
#include "esp_timer.h"

#define LOG_MSG_MAX_LEN 512

static char* TAG = "mqtt_logger";

static const char* level_strings[] = {
    "E", "W", "I", "D"
};

static void log_task(void* pvParameters)
{
    const QueueHandle_t log_queue = queue_manager_get_queue(LOG);
    if (!log_queue)
    {
        ESP_LOGE(TAG, "Failed to get log queue");
        vTaskDelete(NULL);
    }
    while (1)
    {
        QueueMessage_t item;
        if (xQueueReceive(log_queue, &item, portMAX_DELAY) == pdTRUE)
        {
            if (mqtt_publish(TOPIC_ESP32_LOG, item.data, item.len, 0) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to publish log message");
            }
            free_queue_message(&item);
        }
    }
}

void mqtt_logger_init()
{
    if (!queue_manager_init(LOG))
    {
        ESP_LOGE(TAG, "Failed to init log queue");
        return;
    }
    if (xTaskCreate(log_task, "mqtt_logger", 4096, NULL, TASK_PRIORITY_MQTT_LOG, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create log task");
    }
}


void mqtt_log_write(const mqtt_log_level_t level, const char* tag, const char* format, ...)
{
    char* buffer = malloc(LOG_MSG_MAX_LEN);
    if (!buffer) return;

    const QueueHandle_t log_queue = queue_manager_get_queue(LOG);
    if (log_queue == NULL) return;

    va_list args;
    va_start(args, format);

    // 示例格式：[I][main|1630458932] This is a log message
    const int len = snprintf(buffer, LOG_MSG_MAX_LEN, "[%s][%s|%lld] ",
                             level_strings[level],
                             tag,
                             esp_timer_get_time() / 1000000);

    vsnprintf(buffer + len, LOG_MSG_MAX_LEN - len, format, args);
    va_end(args);

    const QueueMessage_t item = {.data = buffer, .len = strlen(buffer), .type = LOG};
    if (xQueueSend(log_queue, &item, 0) != pdTRUE)
    {
        free(buffer); // 队列满时丢弃日志
    }
}
