/* Bounded, epoch-fenced playback. Queue acceptance and codec writes share
 * the cancellation guard; a cancelled producer cannot repopulate the queue.
 * The tail guard covers this board's DMA capacity without stopping RX clocks. */

#include "voice_output.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "task_priorities.h"

#define OUTPUT_CHUNK_FRAMES 320
#define OUTPUT_QUEUE_DEPTH 64
#define OUTPUT_DMA_TAIL_US 110000 /* 6 x 240 / 16k + 20ms margin */

static const char *TAG = "voice_output";

typedef struct {
    uint32_t epoch;
    uint16_t frames;
    int sample_rate;
    int16_t pcm[OUTPUT_CHUNK_FRAMES];
} output_frame_t;

static voice_codec_t *s_codec;
static bool s_enabled;
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
static uint8_t *s_queue_storage;
static SemaphoreHandle_t s_guard;
static uint32_t s_epoch = 1;
static unsigned s_pending;
static int64_t s_tail_until;
static atomic_bool s_failed;
static TaskHandle_t s_task;

static TickType_t wait_ticks(unsigned milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return ticks ? ticks : 1; /* 100Hz: a sub-10ms wait must not become a spin */
}


static void output_task(void *arg)
{
    (void)arg;
    output_frame_t frame;
    while (true) {
        if (xQueueReceive(s_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_guard, portMAX_DELAY);
        bool idle = s_tail_until <= esp_timer_get_time();
        xSemaphoreGive(s_guard);
        if (idle) vTaskDelay(wait_ticks(40)); /* bounded startup prefill */
        xSemaphoreTake(s_guard, portMAX_DELAY);
        if (frame.epoch == s_epoch) {
            /* Flush takes the same guard. Once it returns no old dequeued
             * frame can reach the codec; hardware tail remains bounded. */
            esp_err_t err = voice_codec_write(s_codec, frame.pcm, frame.frames,
                                              frame.sample_rate);
            s_tail_until = esp_timer_get_time() + OUTPUT_DMA_TAIL_US;
            if (err != ESP_OK) {
                atomic_store(&s_failed, true);
                ESP_LOGW(TAG, "codec write failed: %s", esp_err_to_name(err));
            }
            --s_pending;
        }
        xSemaphoreGive(s_guard);
    }
}

esp_err_t voice_output_init(voice_codec_t *codec)
{
    if (codec == NULL || s_codec != NULL) return ESP_ERR_INVALID_ARG;
    s_codec = codec;
    if (!voice_codec_has_output(codec)) {
        ESP_LOGI(TAG, "no playback path on this codec");
        return ESP_OK;
    }

    s_guard = xSemaphoreCreateMutex();
    size_t bytes = OUTPUT_QUEUE_DEPTH * sizeof(output_frame_t);
#if CONFIG_IDF_TARGET_ESP32S3
    /* ~41 KB of jitter buffer; PSRAM keeps internal RAM for the AFE. */
    s_queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue_storage == NULL)
#endif
    s_queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_guard == NULL || s_queue_storage == NULL) goto fail;
    s_queue = xQueueCreateStatic(OUTPUT_QUEUE_DEPTH, sizeof(output_frame_t),
                                 s_queue_storage, &s_queue_control);
    if (s_queue == NULL) goto fail;
    if (xTaskCreate(output_task, "voice_output", 4096, NULL,
                    TASK_PRIO_VOICE_OUTPUT, &s_task) != pdPASS) goto fail;
    s_enabled = true;
    return ESP_OK;
fail:
    voice_output_destroy();
    return ESP_ERR_NO_MEM;
}

bool voice_output_enabled(void) { return s_enabled; }

bool voice_output_failed(void) { return atomic_load(&s_failed); }

uint32_t voice_output_epoch(void)
{
    if (!s_enabled) return 0;
    xSemaphoreTake(s_guard, portMAX_DELAY);
    uint32_t epoch = s_epoch;
    xSemaphoreGive(s_guard);
    return epoch;
}

bool voice_output_active(void)
{
    if (!s_enabled) return false;
    xSemaphoreTake(s_guard, portMAX_DELAY);
    bool active = s_pending > 0 || esp_timer_get_time() < s_tail_until;
    xSemaphoreGive(s_guard);
    return active;
}

void voice_output_flush(void)
{
    if (!s_enabled) return;
    xSemaphoreTake(s_guard, portMAX_DELAY);
    ++s_epoch;
    xQueueReset(s_queue);
    s_pending = 0;
    xSemaphoreGive(s_guard);
    /* Do not reset the hardware tail deadline or stop the shared RX clock. */
    ESP_LOGI(TAG, "playback generation cancelled");
}

esp_err_t voice_output_write_epoch(const int16_t *pcm, size_t frames,
                                   int sample_rate, int channels, uint32_t epoch)
{
    if (!s_enabled) return ESP_OK;
    if (pcm == NULL || frames == 0 || sample_rate != 16000 || channels != 1)
        return ESP_ERR_INVALID_ARG;
    const int64_t deadline = esp_timer_get_time() + 40000;
    while (frames) {
        output_frame_t frame = {.epoch = epoch, .sample_rate = sample_rate,
            .frames = (uint16_t)(frames > OUTPUT_CHUNK_FRAMES ? OUTPUT_CHUNK_FRAMES : frames)};
        memcpy(frame.pcm, pcm, frame.frames * sizeof(int16_t));
        xSemaphoreTake(s_guard, portMAX_DELAY);
        if (epoch != s_epoch) {
            xSemaphoreGive(s_guard);
            return ESP_ERR_INVALID_STATE;
        }
        bool queued = xQueueSend(s_queue, &frame, 0) == pdTRUE;
        if (queued) ++s_pending;
        xSemaphoreGive(s_guard);
        if (!queued) {
            if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
            vTaskDelay(wait_ticks(1));
            continue;
        }
        pcm += frame.frames;
        frames -= frame.frames;
    }
    return ESP_OK;
}

esp_err_t voice_output_write(const int16_t *pcm, size_t frames, int rate, int channels)
{
    return voice_output_write_epoch(pcm, frames, rate, channels, voice_output_epoch());
}

esp_err_t voice_output_wait_drained(unsigned timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (voice_output_active()) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(wait_ticks(5));
    }
    return voice_output_failed() ? ESP_FAIL : ESP_OK;
}

void voice_output_destroy(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    heap_caps_free(s_queue_storage);
    s_queue_storage = NULL;
    if (s_guard) { vSemaphoreDelete(s_guard); s_guard = NULL; }
    s_pending = 0;
    s_tail_until = 0;
    ++s_epoch;
    atomic_store(&s_failed, false);
    s_codec = NULL;
    s_enabled = false;
}
