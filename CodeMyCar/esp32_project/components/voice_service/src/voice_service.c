/**
 * @file voice_service.c
 * @brief I2S 麦克风采集 + WebSocket 推流到 pyserver。
 *
 * 设计：
 * - 默认常驻上传，服务端人声VAD自动分句；串口输入 's' + Enter 触发一次录音（默认 5 秒）；
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
#include "freertos/queue.h"
#include "cJSON.h"
#include "task_priorities.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_service.h"

#define VOICE_BCLK_GPIO 18
#define VOICE_WS_GPIO 19
#define VOICE_DIN_GPIO 21
#define VOICE_SAMPLE_RATE 16000
#define VOICE_CHUNK_FRAMES 320
#define VOICE_WS_CONNECTED_BIT BIT0
#define VOICE_WS_READY_BIT BIT1
#define VOICE_WS_END_BIT BIT2
#define VOICE_WS_FAILED_BIT BIT3
#define VOICE_TASK_STACK 6144
#define VOICE_CONSOLE_STACK 4096

static const char *TAG = "voice";
static i2s_chan_handle_t s_rx;
typedef struct {
    uint32_t sequence;
    uint16_t frames;
    int16_t pcm[VOICE_CHUNK_FRAMES];
} voice_frame_t;
static QueueHandle_t s_ring;
static int32_t *s_raw_buffer;
static int16_t *s_pcm_buffer;
static char s_control[2048];
static int s_control_used;
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
            xEventGroupClearBits(s_ws_events, VOICE_WS_CONNECTED_BIT | VOICE_WS_READY_BIT);
            xEventGroupSetBits(s_ws_events, VOICE_WS_FAILED_BIT);
        }
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
        if (data->op_code == 0x01 && data->data_len > 0) {
            if (data->payload_offset == 0) s_control_used = 0;
            if (data->payload_offset == s_control_used &&
                data->payload_len < sizeof(s_control) &&
                s_control_used + data->data_len < sizeof(s_control)) {
                memcpy(s_control + s_control_used, data->data_ptr, data->data_len);
                s_control_used += data->data_len;
                if (s_control_used == data->payload_len) {
                    s_control[s_control_used] = 0;
                    cJSON *msg = cJSON_Parse(s_control);
                    cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
                    if (cJSON_IsString(type) && s_ws_events) {
                        if (!strcmp(type->valuestring, "ready")) xEventGroupSetBits(s_ws_events, VOICE_WS_READY_BIT);
                        if (!strcmp(type->valuestring, "end")) xEventGroupSetBits(s_ws_events, VOICE_WS_END_BIT);
                        if (!strcmp(type->valuestring, "error")) xEventGroupSetBits(s_ws_events, VOICE_WS_FAILED_BIT);
                    }
                    ESP_LOGI(TAG, "WS <- %s", s_control);
                    cJSON_Delete(msg);
                }
            }
        } else if (data->op_code == 0x02) {
            ESP_LOGI(TAG, "WS <- binary %d bytes (speaker ignored)", data->data_len);
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "websocket error");
        if (s_ws_events) xEventGroupSetBits(s_ws_events, VOICE_WS_FAILED_BIT);
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

static void capture_task(void *arg)
{
    (void)arg;
    int32_t *raw = s_raw_buffer;
    int16_t *pcm = s_pcm_buffer;

    int chunk_count = 0;
    uint32_t sequence = 0;
    uint32_t dropped = 0;
    voice_frame_t frame = {0};
    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx, raw, sizeof(int32_t) * VOICE_CHUNK_FRAMES * 2, &bytes_read,
            portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
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
        frame.sequence = sequence++;
        frame.frames = frames;
        memcpy(frame.pcm, pcm, frames * sizeof(int16_t));
        if (xQueueSend(s_ring, &frame, 0) != pdTRUE) {
            voice_frame_t old;
            xQueueReceive(s_ring, &old, 0); /* idle buffer retains newest frames */
            dropped++;
            xQueueSend(s_ring, &frame, 0);
        }

        chunk_count++;
        if ((chunk_count % 25) == 0) {
            int rms = (int)sqrt((double)sum_sq / frames);
            ESP_LOGI(TAG, "mic rms=%d peak=%d replaced_frames=%lu", rms, peak, (unsigned long)dropped);
        }
        if (!CONFIG_CMCAR_VOICE_CONTINUOUS && !s_busy && (int)sqrt((double)sum_sq / frames) >= CONFIG_CMCAR_VOICE_TRIGGER_RMS) {
            s_trigger = true;
        }
    }
}

static void run_session(void)
{
    if (s_rx == NULL) {
        ESP_LOGE(TAG, "I2S not initialized");
        return;
    }
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "capture ring not initialized");
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
        .disable_auto_reconnect = true,
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
        "{\"type\":\"start\",\"device\":\"%s\",\"rate\":16000,\"ch\":1,\"bits\":16,\"continuous\":%s}",
        CONFIG_CMCAR_DEVICE_ID, CONFIG_CMCAR_VOICE_CONTINUOUS ? "true" : "false");
    if (esp_websocket_client_send_text(ws, start, n, pdMS_TO_TICKS(1000)) != n) {
        ESP_LOGE(TAG, "send start failed");
        goto cleanup;
    }

    bits = xEventGroupWaitBits(s_ws_events, VOICE_WS_READY_BIT | VOICE_WS_FAILED_BIT,
                              pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & VOICE_WS_READY_BIT) || (bits & VOICE_WS_FAILED_BIT)) goto cleanup;
    if (CONFIG_CMCAR_VOICE_CONTINUOUS) xQueueReset(s_ring); /* no replay across reconnect */
    voice_frame_t frame;
    uint32_t expected = 0;
    bool have_sequence = false;
    size_t samples = 0;
    const size_t target_samples = CONFIG_CMCAR_VOICE_RECORD_SECONDS * VOICE_SAMPLE_RATE;
    const TickType_t started = xTaskGetTickCount();
    while (CONFIG_CMCAR_VOICE_CONTINUOUS || samples < target_samples) {
        bits = xEventGroupGetBits(s_ws_events);
        if (!(bits & VOICE_WS_CONNECTED_BIT) || (bits & VOICE_WS_FAILED_BIT)) goto cleanup;
        if (xQueueReceive(s_ring, &frame, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGE(TAG, "capture stalled"); goto cleanup;
        }
        if (have_sequence && frame.sequence != expected) {
            ESP_LOGE(TAG, "audio gap: expected=%lu got=%lu; abort session",
                     (unsigned long)expected, (unsigned long)frame.sequence);
            const char *abort = "{\"type\":\"abort\"}";
            esp_websocket_client_send_text(ws, abort, strlen(abort), pdMS_TO_TICKS(500));
            goto cleanup;
        }
        expected = frame.sequence + 1;
        have_sequence = true;
        size_t count = frame.frames;
        if (!CONFIG_CMCAR_VOICE_CONTINUOUS && count > target_samples - samples)
            count = target_samples - samples;
        int bytes = count * sizeof(int16_t);
        if (esp_websocket_client_send_bin(ws, (const char *)frame.pcm, bytes,
                                         pdMS_TO_TICKS(500)) != bytes) goto cleanup;
        samples += count;
        if (!CONFIG_CMCAR_VOICE_CONTINUOUS &&
            xTaskGetTickCount() - started > pdMS_TO_TICKS((CONFIG_CMCAR_VOICE_RECORD_SECONDS + 3) * 1000))
            goto cleanup;
    }
    const char *end = "{\"type\":\"end\"}";
    if (esp_websocket_client_send_text(ws, end, strlen(end), pdMS_TO_TICKS(1000)) != strlen(end)) goto cleanup;
    xEventGroupWaitBits(s_ws_events, VOICE_WS_END_BIT | VOICE_WS_FAILED_BIT,
                       pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

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

#if CONFIG_CMCAR_VOICE_CONTINUOUS
    while (true) {
        if (wifi_service_is_connected()) {
            s_busy = true;
            run_session();
            s_busy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#elif CONFIG_CMCAR_VOICE_AUTO_LOOP
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

    s_raw_buffer = malloc(sizeof(int32_t) * VOICE_CHUNK_FRAMES * 2);
    s_pcm_buffer = malloc(sizeof(int16_t) * VOICE_CHUNK_FRAMES);
    if (!s_raw_buffer || !s_pcm_buffer) goto task_failure;
    s_ring = xQueueCreate(32, sizeof(voice_frame_t));
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "audio queue alloc failed; voice disabled");
        goto task_failure;
    }

    TaskHandle_t capture = NULL, sender = NULL;
    if (xTaskCreate(capture_task, "voice_capture", 4096, NULL,
                    TASK_PRIO_VOICE_CAPTURE, &capture) != pdPASS) goto task_failure;
    if (xTaskCreate(voice_task, "voice_task", VOICE_TASK_STACK, NULL, 4, &sender) != pdPASS) {
        vTaskDelete(capture);
        goto task_failure;
    }
    if (xTaskCreate(console_task, "voice_console", VOICE_CONSOLE_STACK, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "serial trigger unavailable; network voice still enabled");
    ESP_LOGI(TAG, "voice service started");
    return ESP_OK;
task_failure:
    ESP_LOGE(TAG, "voice task allocation failed; control bridge remains available");
    if (s_ring) vQueueDelete(s_ring);
    s_ring = NULL;
    free(s_raw_buffer);
    free(s_pcm_buffer);
    s_raw_buffer = NULL;
    s_pcm_buffer = NULL;
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return ESP_OK;
}
