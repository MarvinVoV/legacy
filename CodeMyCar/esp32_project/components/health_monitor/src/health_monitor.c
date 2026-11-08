/**
 * @file health_monitor.c
 * @brief 健康监控实现
 *
 * 功能：
 *   - TWDT 看门狗喂养
 *   - 堆内存监控（低于阈值告警/重启）
 *   - MQTT 心跳上报
 */

#include "health_monitor.h"

#include <stdio.h>

#include "cmcar_events.h"
#include "cmcar_topics.h"
#include "bridge.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "task_priorities.h"

static const char* TAG = "health";

#define CHECK_INTERVAL_MS    5000
#define HEARTBEAT_INTERVAL_MS 30000
#define HEAP_WARN_THRESHOLD  20000
#define HEAP_CRIT_THRESHOLD  10000
#define MON_STACK_SIZE       2048

static StackType_t s_mon_stack[MON_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_mon_tcb;
static TaskHandle_t s_mon_handle;
static volatile uint32_t s_connection_generation;

static void on_mqtt_connected(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg; (void)base; (void)id; (void)data;
    ++s_connection_generation;
    if (s_mon_handle != NULL) { xTaskNotifyGive(s_mon_handle); }
}

static void health_mon_task(void* arg)
{
    /* 订阅本喂狗任务自身到 TWDT（必须在任务上下文里调，不能在 app_main 里调
       ——app_main 随后 vTaskDelete(NULL)，否则订阅的是已删除的任务，wdt_reset 空操作） */
    esp_task_wdt_add(NULL);

    uint32_t last_heartbeat = 0;
    uint32_t announced_generation = 0;

    while (1)
    {
        /* Wake immediately on connect/reconnect; failed announcement retries
         * in at most 1s rather than waiting for the periodic 30s heartbeat. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        /* 1. 喂狗 */
        esp_task_wdt_reset();

        /* 2. 堆监控 */
        const size_t heap_free = esp_get_free_heap_size();
        const size_t heap_min = esp_get_minimum_free_heap_size();

        if (heap_free < HEAP_CRIT_THRESHOLD)
        {
            ESP_LOGE(TAG, "CRITICAL: heap %zu bytes, restarting!", heap_free);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        else if (heap_free < HEAP_WARN_THRESHOLD)
        {
            ESP_LOGW(TAG, "Low heap: %zu bytes (min ever: %zu)",
                     heap_free, heap_min);
            cmcar_health_warning_t warn = {
                .heap_free = (uint32_t)heap_free,
                .heap_min_ever = (uint32_t)heap_min,
            };
            esp_event_post(CMCAR_EVENTS, CMCAR_HEALTH_WARNING,
                           &warn, sizeof(warn), 0);
        }

        /* 3. 心跳 */
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t generation = s_connection_generation;
        if (generation != announced_generation ||
            now_ms - last_heartbeat >= HEARTBEAT_INTERVAL_MS)
        {
            if (mqtt_service_is_connected())
            {
                char json[320];
                const esp_app_desc_t* app = esp_app_get_description();
                const int len = snprintf(
                    json, sizeof(json),
                    "{\"heap\":%zu,\"heap_min\":%zu,\"uptime_s\":%lld,\"stm32_connected\":%s,\"session_id\":%lu,"
                    "\"esp_build\":\"%s\",\"esp_idf\":\"%s\",\"stm32_build\":\"%s\","
                    "\"stm32_dirty\":%s,\"protocol_version\":%u}",
                    heap_free, heap_min,
                    (long long)(esp_timer_get_time() / 1000000),
                    bridge_stm32_connected() ? "true" : "false",
                    (unsigned long)bridge_session_id(), app->version,
                    esp_get_idf_version(), bridge_stm32_build_sha(),
                    bridge_stm32_build_dirty() ? "true" : "false",
                    (unsigned)bridge_stm32_protocol_version());

                /* 用超时发布：broker 卡住时不能阻塞喂狗任务，否则 TWDT 自触 panic */
                const esp_err_t pub = (len > 0 && (size_t)len < sizeof(json))
                    ? mqtt_service_publish_timed(
                        CMCAR_TOPIC_HEARTBEAT, json, (size_t)len, 1, 1000)
                    : ESP_ERR_INVALID_SIZE;
                if (pub == ESP_OK)
                {
                    last_heartbeat = now_ms;
                    announced_generation = generation;
                    ESP_LOGI(TAG, "Session announced: %lu", (unsigned long)bridge_session_id());
                }
                if (pub != ESP_OK)
                {
                    ESP_LOGD(TAG, "Heartbeat publish skipped: %s",
                             esp_err_to_name(pub));
                }
                ESP_LOGD(TAG, "Heartbeat: heap=%zu, uptime=%llds",
                         heap_free, (long long)(esp_timer_get_time() / 1000000));
            }
        }
    }
}

esp_err_t health_monitor_init(void)
{
    /* 初始化任务看门狗 */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&wdt_cfg);
    if (err == ESP_ERR_INVALID_STATE)
    {
        /* CONFIG_ESP_TASK_WDT_INIT=y（IDF 默认）时启动阶段已初始化过 TWDT，
           此处不能重复 init，只能改配置 */
        err = esp_task_wdt_reconfigure(&wdt_cfg);
    }
    if (err != ESP_OK) { return err; }
    return esp_event_handler_register(CMCAR_EVENTS, CMCAR_MQTT_CONNECTED,
                                     on_mqtt_connected, NULL);
}

esp_err_t health_monitor_start(void)
{
    s_mon_handle = xTaskCreateStatic(
        health_mon_task, "health_mon",
        MON_STACK_SIZE / sizeof(StackType_t),
        NULL, TASK_PRIO_HEALTH, s_mon_stack, &s_mon_tcb);

    ESP_LOGI(TAG, "Health monitor started");
    return (s_mon_handle != NULL) ? ESP_OK : ESP_FAIL;
}

size_t health_monitor_get_heap_free(void)
{
    return esp_get_free_heap_size();
}

size_t health_monitor_get_heap_min(void)
{
    return esp_get_minimum_free_heap_size();
}
