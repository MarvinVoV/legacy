#include "app_events.h"

#include "esp_log.h"
#include "freertos/event_groups.h"

// 定义全局事件组
static EventGroupHandle_t g_app_event_group = NULL;
// 定义全局互斥量
static SemaphoreHandle_t g_app_event_group_mutex = NULL;

// 定义日志标签
static const char* TAG = "AppEvents";

/* 初始化应用程序事件系统 */
void app_events_init(void)
{
    if (g_app_event_group == NULL)
    {
        // 防止重复初始化
        g_app_event_group = xEventGroupCreate();
        if (g_app_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create event group!");
        }
    }
    // 创建互斥量
    if (g_app_event_group_mutex == NULL)
    {
        g_app_event_group_mutex = xSemaphoreCreateMutex();
        if (g_app_event_group_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create event group mutex!");
        }
    }
}

/* 获取应用事件组句柄 */
EventGroupHandle_t app_events_get_group(void)
{
    return g_app_event_group;
}

/* 清除事件标志 */
void app_events_clear_bits(EventBits_t bits)
{
    xEventGroupClearBits(g_app_event_group, bits);
}

/* 获取当前事件标志 */
EventBits_t app_events_get_bits(void)
{
    if (g_app_event_group == NULL)
    {
        ESP_LOGE(TAG, "Event group not initialized!");
        return 0;
    }
    return xEventGroupGetBits(g_app_event_group);
}

void app_events_set_bits(const EventBits_t bits)
{
    if (g_app_event_group == NULL)
    {
        ESP_LOGE(TAG, "Event group not initialized!");
        return;
    }
    xEventGroupSetBits(g_app_event_group, bits);
}

/**
 * 原子地清除某些事件位并设置另一些事件位
 * @param bits_to_clear 要清除的事件位
 * @param bits_to_set 要设置的事件位
 */
void app_events_set_bits_atomic(const EventBits_t bits_to_clear,
                                const EventBits_t bits_to_set)
{
    if (g_app_event_group_mutex == NULL || g_app_event_group == NULL)
    {
        ESP_LOGE(TAG, "Event group or mutex not initialized!");
        return;
    }

    // 进入临界区
    if (xSemaphoreTake(g_app_event_group_mutex, portMAX_DELAY) == pdTRUE)
    {
        xEventGroupClearBits(g_app_event_group, bits_to_clear);
        xEventGroupSetBits(g_app_event_group, bits_to_set);
        xSemaphoreGive(g_app_event_group_mutex); // 退出临界区
    }
    else
    {
        ESP_LOGE(TAG, "Failed to take event group mutex!");
    }
}
