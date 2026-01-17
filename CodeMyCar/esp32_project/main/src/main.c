#include "app_events.h"
#include "esp_log.h"
#include "mqtt_manager.h"
#include "nvs_flash.h"
#include "queue_manager.h"
#include "uart_manager.h"
#include "wifi_manager.h"
#include "mqtt_logger.h"
#include "task_priorities.h"

const char* TAG = "APP";

static void on_wifi_connected();
static void on_wifi_got_ip();
static void on_mqtt_connected();

void app_main(void)
{
    // 初始化基础组件
    nvs_flash_init();
    app_events_init();

    // 初始化队列资源
    queue_manager_full_init();

    // 初始化串口管理器
    uart_init();
    // 初始化模块
    wifi_init_and_start();

    // 主循环：监听事件组状态
    while (1)
    {
        const EventBits_t bits = xEventGroupWaitBits(
            app_events_get_group(),
            EVENT_WIFI_CONNECTED | EVENT_WIFI_GOT_IP | EVENT_WIFI_DISCONNECTED |
            UART_INIT_COMPLETE | EVENT_MQTT_CONNECTED,
            pdTRUE,  // 自动清除事件位
            pdFALSE, // 等待任意事件位发生
            pdMS_TO_TICKS(100));

        if (bits & EVENT_WIFI_CONNECTED)
        {
            on_wifi_connected();
        }
        if (bits & EVENT_WIFI_DISCONNECTED)
        {
            // print log
            ESP_LOGI(TAG, "WiFi disconnected");
        }
        if (bits & EVENT_WIFI_GOT_IP)
        {
            on_wifi_got_ip();
        }
        if (bits & UART_INIT_COMPLETE)
        {
            ESP_LOGI(TAG, "UART init complete");
        }
        if (bits & EVENT_MQTT_CONNECTED)
        {
            on_mqtt_connected();
        }
    }
}

// WiFi连接成功后的回调函数
static void on_wifi_connected()
{
    ESP_LOGI(TAG, "WiFi connected. Heap free: %d", esp_get_free_heap_size());
}

// 获取IP地址后的回调函数
static void on_wifi_got_ip()
{
    ESP_LOGI(TAG, "Got IP. Heap free: %d", esp_get_free_heap_size());
    // 初始化MQTT
    mqtt_init_and_start();
}

static void on_mqtt_connected()
{
    ESP_LOGI(TAG, "MQTT connected. Heap free: %d", esp_get_free_heap_size());
    if (xTaskCreate(handle_uart_message_task, "handle_uart_message_task", 4096,
                    NULL, TASK_PRIORITY_MQTT_HANDLE_UART_MSG, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create handle_uart_message_task task");
    }
    else
    {
        // TODO change log level to debug
        ESP_LOGI(TAG, "Created handle_uart_message_task task");
    }

    // 初始化日志模块
    mqtt_logger_init();
    vTaskDelay(pdMS_TO_TICKS(500));
}
