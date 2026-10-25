/**
 * @file wifi_service.c
 * @brief WiFi STA 服务实现
 *
 * 事件驱动：WiFi/IP 事件 → 发布 CMCAR_EVENTS → 上层组件响应
 * 指数退避重连：1s → 2s → 4s → ... → 60s
 */

#include "wifi_service.h"

#include <string.h>

#include "cmcar_events.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "wifi_svc";

#define WIFI_RECONNECT_MAX_MS 60000

static bool s_connected = false;
static int s_retry_count = 0;

/* 指数退避计算 */
static uint32_t backoff_ms(int attempt)
{
    uint32_t delay = CONFIG_CMCAR_WIFI_RECONNECT_BASE_MS;
    for (int i = 0; i < attempt && delay < WIFI_RECONNECT_MAX_MS; i++)
    {
        delay *= 2;
    }
    return (delay > WIFI_RECONNECT_MAX_MS) ? WIFI_RECONNECT_MAX_MS : delay;
}

/* WiFi + IP 事件处理器 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Associated with AP");
                s_retry_count = 0;
                s_connected = true;
                esp_event_post(CMCAR_EVENTS, CMCAR_WIFI_CONNECTED, NULL, 0,
                               pdMS_TO_TICKS(100));
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                s_connected = false;
                const wifi_event_sta_disconnected_t* evt =
                    (wifi_event_sta_disconnected_t*)event_data;
                ESP_LOGW(TAG, "Disconnected (reason: %d)", evt->reason);
                esp_event_post(CMCAR_EVENTS, CMCAR_WIFI_DISCONNECTED, NULL, 0,
                               pdMS_TO_TICKS(100));

                /* 指数退避重连 */
                const int max_retry = CONFIG_CMCAR_WIFI_MAX_RETRY;
                if (max_retry == 0 || s_retry_count < max_retry)
                {
                    const uint32_t delay = backoff_ms(s_retry_count);
                    ESP_LOGI(TAG, "Reconnecting in %lu ms (attempt %d)",
                             (unsigned long)delay, s_retry_count + 1);
                    vTaskDelay(pdMS_TO_TICKS(delay));
                    s_retry_count++;
                    esp_wifi_connect();
                }
                else
                {
                    ESP_LOGE(TAG, "Max retries (%d) reached, giving up",
                             max_retry);
                }
                break;
            }

            default:
                break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* evt = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;

        cmcar_wifi_ip_event_t ip_evt = {
            .ip_addr = evt->ip_info.ip.addr,
        };
        esp_event_post(CMCAR_EVENTS, CMCAR_WIFI_GOT_IP, &ip_evt,
                       sizeof(ip_evt), pdMS_TO_TICKS(100));
    }
}

esp_err_t wifi_service_init(void)
{
    esp_err_t ret;

    /* 初始化 TCP/IP 栈 */
    ret = esp_netif_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建默认 STA 网络接口 */
    esp_netif_create_default_wifi_sta();

    /* 初始化 WiFi 驱动 */
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 注册事件处理器 */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    /* 设置 STA 模式 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi service initialized");
    return ESP_OK;
}

esp_err_t wifi_service_start(void)
{
    /* 配置 WiFi 参数 */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_CMCAR_WIFI_SSID,
            .password = CONFIG_CMCAR_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi connecting to \"%s\"...", CONFIG_CMCAR_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_service_stop(void)
{
    s_connected = false;
    return esp_wifi_stop();
}

bool wifi_service_is_connected(void)
{
    return s_connected;
}
