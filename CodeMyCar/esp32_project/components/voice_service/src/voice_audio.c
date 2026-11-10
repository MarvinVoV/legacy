/* I2S/AFE resource ownership. No network or cloud dependency. */
#include "voice_audio.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "voice_frontend.h"
#else
#include "voice_wake.h"
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "task_priorities.h"

#define VOICE_BCLK_GPIO CONFIG_CMCAR_VOICE_I2S_BCLK
#define VOICE_WS_GPIO CONFIG_CMCAR_VOICE_I2S_WS
#define VOICE_DIN_GPIO CONFIG_CMCAR_VOICE_I2S_DIN
#if CONFIG_CMCAR_VOICE_I2S_RIGHT_CHANNEL
#define VOICE_INPUT_SLOT 1
#else
#define VOICE_INPUT_SLOT 0
#endif
static const char *TAG = "voice_audio";
static i2s_chan_handle_t s_rx;
static QueueHandle_t s_ring;
static StaticQueue_t s_ring_control;
static uint8_t *s_ring_storage;
static int32_t *s_raw_buffer;
static int16_t *s_pcm_buffer;
static atomic_bool s_active;
static atomic_bool s_energy_trigger;
static atomic_uint s_replaced_frames;
static EventGroupHandle_t s_gate;
static StaticEventGroup_t s_gate_control;
static TaskHandle_t s_capture_task;
#if CONFIG_IDF_TARGET_ESP32S3
static TaskHandle_t s_frontend_task;
#endif

static void queue_pcm(voice_frame_t *frame)
{
    if (xQueueSend(s_ring, frame, 0) != pdTRUE) {
        voice_frame_t old;
        xQueueReceive(s_ring, &old, 0);
        atomic_fetch_add(&s_replaced_frames, 1);
        xQueueSend(s_ring, frame, 0);
    }
}

#if CONFIG_IDF_TARGET_ESP32S3
static void frontend_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_gate, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);
    voice_frame_t frame = {0};
    uint32_t sequence = 0;
    bool previous_speech = false;
    bool pending_wake = false;
    while (true) {
        const int16_t *pcm;
        size_t samples;
        bool wake, speech;
        if (!voice_frontend_fetch(&pcm, &samples, &wake, &speech, !atomic_load(&s_active))) {
            /* A timeout/error breaks stream continuity; never join old partial
             * PCM to new PCM and silently recognise a corrupted command. */
            frame.frames = 0;
            pending_wake = false;
            ++sequence;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (speech != previous_speech) {
            ESP_LOGI(TAG, "AFE VAD %s", speech ? "speech" : "silence");
            previous_speech = speech;
        }
        if (wake && CONFIG_CMCAR_VOICE_WAKE_ENABLED && !atomic_load(&s_active)) {
            pending_wake = true;
            ESP_LOGI(TAG, "DETECTED nihaoxiaozhi");
        }
        while (samples) {
            size_t n = VOICE_CHUNK_FRAMES - frame.frames;
            if (n > samples) n = samples;
            memcpy(frame.pcm + frame.frames, pcm, n * sizeof(int16_t));
            frame.frames += n; pcm += n; samples -= n;
            if (frame.frames == VOICE_CHUNK_FRAMES) {
                frame.sequence = sequence++;
                frame.wake = pending_wake;
                pending_wake = false;
                queue_pcm(&frame);
                frame.frames = 0;
            }
        }
    }
}
#endif

static esp_err_t i2s_rx_init(void)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(VOICE_BCLK_GPIO) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(VOICE_WS_GPIO) ||
        !GPIO_IS_VALID_GPIO(VOICE_DIN_GPIO) ||
        VOICE_BCLK_GPIO == VOICE_WS_GPIO || VOICE_BCLK_GPIO == VOICE_DIN_GPIO ||
        VOICE_WS_GPIO == VOICE_DIN_GPIO ||
        VOICE_BCLK_GPIO == CONFIG_CMCAR_UART_TX_PIN || VOICE_BCLK_GPIO == CONFIG_CMCAR_UART_RX_PIN ||
        VOICE_WS_GPIO == CONFIG_CMCAR_UART_TX_PIN || VOICE_WS_GPIO == CONFIG_CMCAR_UART_RX_PIN ||
        VOICE_DIN_GPIO == CONFIG_CMCAR_UART_TX_PIN || VOICE_DIN_GPIO == CONFIG_CMCAR_UART_RX_PIN) {
        ESP_LOGE(TAG, "Invalid I2S pins or collision with control UART");
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    const int pins[] = {VOICE_BCLK_GPIO, VOICE_WS_GPIO, VOICE_DIN_GPIO};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] >= 26 && pins[i] <= 37) {
            ESP_LOGE(TAG, "I2S pin conflicts with S3 WROOM-2 Flash/PSRAM");
            return ESP_ERR_INVALID_ARG;
        }
    }
#endif
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
    /* ICS43434 releases SD in the unused slot. Also keep an unplugged input
     * quiet instead of feeding floating-pin noise into the wake model. */
    gpio_set_pull_mode(VOICE_DIN_GPIO, GPIO_PULLDOWN_ONLY);
    return i2s_channel_enable(s_rx);
}

static void capture_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_gate, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);
    int32_t *raw = s_raw_buffer;
    int16_t *pcm = s_pcm_buffer;

    int chunk_count = 0;
#if !CONFIG_IDF_TARGET_ESP32S3
    uint32_t sequence = 0;
    voice_frame_t frame = {0};
#endif
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
            int32_t v = raw[2 * i + VOICE_INPUT_SLOT] >> 16;
            pcm[i] = (int16_t)v;
            sum_sq += (int64_t)v * v;
            int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
#if CONFIG_IDF_TARGET_ESP32S3
        if (!voice_frontend_feed(pcm, (size_t)frames)) {
            ESP_LOGE(TAG, "AFE feed failed");
            vTaskDelay(pdMS_TO_TICKS(20));
        }
#else
        frame.sequence = sequence++;
        frame.frames = frames;
        memcpy(frame.pcm, pcm, frames * sizeof(int16_t));
        queue_pcm(&frame);
#endif

        chunk_count++;
        if ((chunk_count % 25) == 0) {
            int rms = (int)sqrt((double)sum_sq / frames);
            ESP_LOGI(TAG, "mic rms=%d peak=%d replaced_frames=%u", rms, peak,
                     (unsigned)atomic_load(&s_replaced_frames));
        }
        if (!CONFIG_CMCAR_VOICE_WAKE_ENABLED && !CONFIG_CMCAR_VOICE_CONTINUOUS && !atomic_load(&s_active) && (int)sqrt((double)sum_sq / frames) >= CONFIG_CMCAR_VOICE_TRIGGER_RMS) {
            atomic_store(&s_energy_trigger, true);
        }
    }
}

bool voice_audio_receive(voice_frame_t *frame, unsigned timeout_ms)
{
    return s_ring && xQueueReceive(s_ring, frame, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void voice_audio_flush(void) { if (s_ring) xQueueReset(s_ring); }
void voice_audio_set_active(bool active) { atomic_store(&s_active, active); }
bool voice_audio_take_energy_trigger(void) { return atomic_exchange(&s_energy_trigger, false); }

bool voice_audio_detect_wake(const voice_frame_t *frame)
{
#if CONFIG_IDF_TARGET_ESP32S3
    return frame->wake;
#else
    return voice_wake_feed(frame->pcm, frame->frames, frame->sequence);
#endif
}

void voice_audio_reset_wake(void)
{
#if !CONFIG_IDF_TARGET_ESP32S3
    voice_wake_reset();
#endif
}

void voice_audio_destroy(void)
{
    /* Only called before start or after a failed start whose tasks stayed
     * behind s_gate. Never delete an owner midway through an AFE operation. */
    if (s_capture_task) { vTaskDelete(s_capture_task); s_capture_task = NULL; }
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_frontend_task) { vTaskDelete(s_frontend_task); s_frontend_task = NULL; }
    voice_frontend_destroy();
#else
    voice_wake_destroy();
#endif
    if (s_gate) { vEventGroupDelete(s_gate); s_gate = NULL; }
    if (s_ring) { vQueueDelete(s_ring); s_ring = NULL; }
    heap_caps_free(s_ring_storage); s_ring_storage = NULL;
    heap_caps_free(s_raw_buffer); s_raw_buffer = NULL;
    heap_caps_free(s_pcm_buffer); s_pcm_buffer = NULL;
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

esp_err_t voice_audio_init(void)
{
    if (s_rx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_rx_init();
    if (err != ESP_OK) { voice_audio_destroy(); return err; }
    s_gate = xEventGroupCreateStatic(&s_gate_control);
    s_raw_buffer = heap_caps_malloc(sizeof(int32_t) * VOICE_CHUNK_FRAMES * 2,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_pcm_buffer = heap_caps_malloc(sizeof(int16_t) * VOICE_CHUNK_FRAMES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t queue_bytes = CONFIG_CMCAR_VOICE_QUEUE_FRAMES * sizeof(voice_frame_t);
#if CONFIG_IDF_TARGET_ESP32S3
    s_ring_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_ring_storage = heap_caps_malloc(queue_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (!s_gate || !s_raw_buffer || !s_pcm_buffer || !s_ring_storage) goto fail;
    s_ring = xQueueCreateStatic(CONFIG_CMCAR_VOICE_QUEUE_FRAMES, sizeof(voice_frame_t),
                                s_ring_storage, &s_ring_control);
    if (!s_ring) goto fail;
#if CONFIG_IDF_TARGET_ESP32S3
    if (!voice_frontend_init()) goto fail;
#else
    if (!voice_wake_init()) goto fail;
#endif
    ESP_LOGI(TAG, "I2S BCLK=%d WS=%d DIN=%d slot=%s queue_bytes=%u",
             VOICE_BCLK_GPIO, VOICE_WS_GPIO, VOICE_DIN_GPIO,
             VOICE_INPUT_SLOT ? "right" : "left", (unsigned)queue_bytes);
    return ESP_OK;
fail:
    voice_audio_destroy();
    return ESP_ERR_NO_MEM;
}

esp_err_t voice_audio_start(void)
{
    if (!s_gate || !s_ring || s_capture_task) return ESP_ERR_INVALID_STATE;
#if CONFIG_IDF_TARGET_ESP32S3
    if (xTaskCreate(frontend_task, "voice_afe_out", 6144, NULL,
                    TASK_PRIO_VOICE_FRONTEND, &s_frontend_task) != pdPASS) return ESP_ERR_NO_MEM;
#endif
    if (xTaskCreate(capture_task, "voice_capture", 4096, NULL,
                    TASK_PRIO_VOICE_CAPTURE, &s_capture_task) != pdPASS) return ESP_ERR_NO_MEM;
    xEventGroupSetBits(s_gate, BIT0);
    return ESP_OK;
}
