/**
 * @file voice_service.c
 * @brief I2S 麦克风采集 + WebSocket 推流到 pyserver。
 *
 * 设计：
 * - 串口输入 's' + Enter 触发一次录音（默认 5 秒）；
 * - I2S RX 16 kHz / 32-bit slot / stereo，取左声道转 16-bit mono；
 * - 通过 esp_websocket_client 发 start -> PCM 二进制帧 -> end；
 * - 服务端返回的 final/action/speak 文本打印到串口；
 * - 音频任务优先级低于 bridge，绝不阻塞 UART/MQTT/STOP。
 */

#include "voice_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_service.h"

#define VOICE_BCLK_GPIO 18
#define VOICE_WS_GPIO 19
#define VOICE_DIN_GPIO 21
#define VOICE_SAMPLE_RATE 16000
#define VOICE_CHUNK_FRAMES 320
#define VOICE_WS_CONNECTED_BIT BIT0
#define VOICE_TASK_STACK 6144
#define VOICE_CONSOLE_STACK 4096

static const char *TAG = "voice";
static i2s_chan_handle_t s_rx;
static EventGroupHandle_t s_ws_events;
static volatile bool s_trigger;
static volatile bool s_busy;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "websocket connected");
        if (s_ws_events != NULL) {
            xEventGroupSetBits(s_ws_events, VOICE_WS_CONNECTED_BIT);
        }
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "websocket disconnected");
        if (s_ws_events != NULL) {
            xEventGroupClearBits(s_ws_events, VOICE_WS_CONNECTED_BIT);
        }
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
        if (data->op_code == 0x01 && data->data_len > 0) {
            ESP_LOGI(TAG, "WS <- %.*s", data->data_len, data->data_ptr);
        } else if (data->op_code == 0x02) {
            ESP_LOGI(TAG, "WS <- binary %d bytes (speaker ignored)", data->data_len);
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "websocket error");
    }
}

static esp_err_t i2s_rx_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = VOICE_BCLK_GPIO,
            .ws = VOICE_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = VOICE_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_rx);
}

static void run_session(void)
{
    if (s_rx == NULL) {
        ESP_LOGE(TAG, "I2S not initialized");
        return;
    }

    s_ws_events = xEventGroupCreate();
    if (s_ws_events == NULL) {
        ESP_LOGE(TAG, "event group alloc failed");
        return;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = CONFIG_CMCAR_VOICE_WS_URL,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        .buffer_size = 2048,
        .task_stack = 4096,
        .task_prio = 4,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&ws_cfg);
    if (ws == NULL) {
        ESP_LOGE(TAG, "websocket init failed");
        vEventGroupDelete(s_ws_events);
        s_ws_events = NULL;
        return;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(ws);
        vEventGroupDelete(s_ws_events);
        s_ws_events = NULL;
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_ws_events, VOICE_WS_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    if ((bits & VOICE_WS_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "websocket connect timeout");
        goto cleanup;
    }

    char start[192];
    int n = snprintf(
        start, sizeof(start),
        "{\"type\":\"start\",\"device\":\"%s\",\"rate\":16000,\"ch\":1,\"bits\":16}",
        CONFIG_CMCAR_DEVICE_ID);
    if (esp_websocket_client_send_text(ws, start, n, pdMS_TO_TICKS(1000)) < 0) {
        ESP_LOGE(TAG, "send start failed");
        goto cleanup;
    }

    int32_t *raw = malloc(sizeof(int32_t) * VOICE_CHUNK_FRAMES * 2);
    int16_t *pcm = malloc(sizeof(int16_t) * VOICE_CHUNK_FRAMES);
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "audio buffer alloc failed");
        free(raw);
        free(pcm);
        goto cleanup;
    }

    int chunks = (CONFIG_CMCAR_VOICE_RECORD_SECONDS * 1000) / 20;
    for (int c = 0; c < chunks; c++) {
        size_t bytes_read = 0;
        err = i2s_channel_read(
            s_rx, raw, sizeof(int32_t) * VOICE_CHUNK_FRAMES * 2, &bytes_read,
            pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(err));
            break;
        }
        int frames = (int)(bytes_read / (sizeof(int32_t) * 2));
        if (frames <= 0) {
            continue;
        }
        int64_t sum_sq = 0;
        int peak = 0;
        for (int i = 0; i < frames; i++) {
            int32_t v = raw[2 * i] >> 16;
            pcm[i] = (int16_t)v;
            sum_sq += (int64_t)v * v;
            int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        if ((c % 25) == 0) {
            int rms = (int)sqrt((double)sum_sq / frames);
            ESP_LOGI(TAG, "mic rms=%d peak=%d", rms, peak);
        }
        int sent = esp_websocket_client_send_bin(
            ws, (const char *)pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(1000));
        if (sent < 0) {
            ESP_LOGE(TAG, "send pcm failed");
            break;
        }
    }

    free(raw);
    free(pcm);
    const char *end = "{\"type\":\"end\"}";
    esp_websocket_client_send_text(ws, end, (int)strlen(end), pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(2000));

cleanup:
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    vEventGroupDelete(s_ws_events);
    s_ws_events = NULL;
}

static void voice_task(void *arg)
{
    (void)arg;
    while (!wifi_service_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

#if CONFIG_CMCAR_VOICE_AUTO_LOOP
    ESP_LOGI(TAG, "auto loop enabled; first session in 3 s");
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        ESP_LOGI(TAG, "auto session start");
        run_session();
        ESP_LOGI(TAG, "auto session done; next in %d s", CONFIG_CMCAR_VOICE_LOOP_DELAY_SEC);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CMCAR_VOICE_LOOP_DELAY_SEC * 1000));
    }
#else
    while (true) {
        if (!s_trigger) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_trigger = false;
        if (s_busy) {
            continue;
        }
        s_busy = true;
        if (!wifi_service_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected; voice upload skipped");
        } else {
            ESP_LOGI(TAG, "voice session start");
            run_session();
            ESP_LOGI(TAG, "voice session done");
        }
        s_busy = false;
    }
#endif
}

static void console_task(void *arg)
{
    (void)arg;
    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART0 driver install failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "voice ready: type 's' + Enter to record %d s",
             CONFIG_CMCAR_VOICE_RECORD_SECONDS);

    while (true) {
        uint8_t c = 0;
        int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == 's' || c == 'S') {
            if (s_busy) {
                ESP_LOGW(TAG, "voice session already running");
            } else {
                s_trigger = true;
                ESP_LOGI(TAG, "voice trigger");
            }
        }
    }
}

esp_err_t voice_service_start(void)
{
    esp_err_t err = i2s_rx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s; voice disabled", esp_err_to_name(err));
        return ESP_OK; /* 不阻塞 bridge */
    }

    xTaskCreate(voice_task, "voice_task", VOICE_TASK_STACK, NULL, 4, NULL);
    xTaskCreate(console_task, "voice_console", VOICE_CONSOLE_STACK, NULL, 2, NULL);
    ESP_LOGI(TAG, "voice service started");
    return ESP_OK;
}
