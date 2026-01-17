#include "wifi_manager.h"

#include "app_events.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "task_priorities.h"

#define TAG "WiFiManager"
#define MAX_RETRIES 5

// WiFi事件组句柄
static EventGroupHandle_t s_wifi_event_group = NULL;
// WiFi连接尝试次数
static int s_retry_count = 0;
// WiFi任务是否正在运行
static bool s_wifi_task_running = false;

// WiFi事件处理函数
static void wifi_event_handler(void* arg, const esp_event_base_t event_base,
                               const int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to AP");
                s_retry_count = 0;
                app_events_set_bits_atomic(EVENT_WIFI_DISCONNECTED,
                                           EVENT_WIFI_CONNECTED);
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    const wifi_event_sta_disconnected_t* event =
                        (wifi_event_sta_disconnected_t*)event_data;
                    ESP_LOGW(TAG, "Disconnected reason: %d", event->reason);

                    if (s_retry_count < MAX_RETRIES)
                    {
                        esp_wifi_connect();
                        s_retry_count++;
                        ESP_LOGI(TAG, "Retrying connection (attempt %d/%d)", s_retry_count,
                                 MAX_RETRIES);
                    }
                    else
                    {
                        app_events_set_bits_atomic(EVENT_WIFI_CONNECTED | EVENT_WIFI_GOT_IP,
                                                   EVENT_WIFI_DISCONNECTED);
                    }
                    break;
                }

            default:
                break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            const ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            app_events_set_bits_atomic(EVENT_WIFI_DISCONNECTED, EVENT_WIFI_GOT_IP);
        }
    }
}

// WiFi连接任务
static void wifi_connect_task(void* pvParameters)
{
    // 注册WiFi和IP事件处理函数
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);
    // 配置WiFi参数
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_EXAMPLE_WIFI_SSID,
            .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    // 设置WiFi配置并启动WiFi
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 标记任务已结束
    s_wifi_task_running = false;

    // 删除任务
    vTaskDelete(NULL);
}

// 初始化WiFi
void wifi_init(void)
{
    // 初始化事件组
    if (s_wifi_event_group == NULL)
    {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create WiFi event group!");
            return;
        }
    }

    // 初始化底层TCP/IP堆栈
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认的事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建默认的Wi-Fi Station网络接口
    esp_netif_create_default_wifi_sta();

    // 使用默认配置初始化Wi-Fi驱动
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 设置Wi-Fi工作模式为Station模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

// 开始WiFi连接
void wifi_start_connect(void)
{
    if (!s_wifi_task_running)
    {
        s_wifi_task_running = true;
        // 创建WiFi连接任务
        xTaskCreate(wifi_connect_task, "wifi_conn", 4096, NULL,
                    TASK_PRIORITY_WIFI_MANAGER, NULL);
    }
    else
    {
        ESP_LOGW(TAG, "WiFi connect task is already running!");
    }
}

// 停止WiFi连接
void wifi_stop_connect(void)
{
    // 停止WiFi
    ESP_ERROR_CHECK(esp_wifi_stop());
}

// 初始化并启动WiFi模块（合并方法）
void wifi_init_and_start(void)
{
    // 初始化WiFi模块
    wifi_init();

    // 启动WiFi连接
    wifi_start_connect();
}
