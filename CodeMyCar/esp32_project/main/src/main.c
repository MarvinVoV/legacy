/**
 * @file main.c
 * @brief CodeMyCar ESP32-C6 固件 v2.0 入口
 *
 * 事件驱动架构：app_main 完成初始化后删除自身，
 * 所有工作由各组件任务和 esp_event 事件循环驱动。
 */

#include "bridge.h"
#include "cmcar_events.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_monitor.h"
#include "mqtt_service.h"
#include "nvs_flash.h"
#include "remote_log.h"
#include "uart_service.h"
#include "wifi_service.h"

static const char* TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== CodeMyCar ESP32-C6 v2.0 ===");
    ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());

    /* 1. NVS 初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 创建默认事件循环（WiFi/MQTT 系统事件需要） */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. 按依赖顺序初始化各组件 */
    ESP_ERROR_CHECK(health_monitor_init());
    ESP_LOGI(TAG, "[1/7] health_monitor initialized");

    ESP_ERROR_CHECK(uart_service_init());
    ESP_LOGI(TAG, "[2/7] uart_service initialized");

    ESP_ERROR_CHECK(wifi_service_init());
    ESP_LOGI(TAG, "[3/7] wifi_service initialized");

    ESP_ERROR_CHECK(mqtt_service_init());
    ESP_LOGI(TAG, "[4/7] mqtt_service initialized");

    ESP_ERROR_CHECK(remote_log_init());
    ESP_LOGI(TAG, "[5/7] remote_log initialized");

    ESP_ERROR_CHECK(bridge_init());
    ESP_LOGI(TAG, "[6/7] bridge initialized");

    /* 4. 启动各组件 */
    ESP_ERROR_CHECK(uart_service_start());
    ESP_ERROR_CHECK(bridge_start());
    ESP_ERROR_CHECK(remote_log_start());
    ESP_ERROR_CHECK(health_monitor_start());
    ESP_LOGI(TAG, "[7/7] all services started");

    /* 5. 启动 WiFi（后续由事件驱动 MQTT 连接和桥接） */
    ESP_ERROR_CHECK(wifi_service_start());

    ESP_LOGI(TAG, "System ready. Free heap: %zu bytes",
             esp_get_free_heap_size());

    RLOG_I(TAG, "System booted, heap=%lu",
           (unsigned long)esp_get_free_heap_size());

    /* 6. app_main 任务删除自身，所有工作由事件循环和各任务驱动 */
    ESP_LOGI(TAG, "app_main complete, deleting self");
    vTaskDelete(NULL);
}
