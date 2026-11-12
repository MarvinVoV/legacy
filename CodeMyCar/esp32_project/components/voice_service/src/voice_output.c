/* Bounded playback queue between the WebSocket task and the board codec.
 *
 * The network task never touches I2S directly. When the queue is full it
 * waits for a frame to drain (bounded, 500 ms) so TCP backpressure paces a
 * long TTS phrase instead of truncating it. A permanently stalled output task
 * still turns into a bounded error rather than an unbounded session stall. */

#include "voice_output.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "task_priorities.h"

#define OUTPUT_CHUNK_FRAMES 320
#define OUTPUT_QUEUE_DEPTH 64
#define OUTPUT_DRAINED_BIT BIT0

static const char *TAG = "voice_output";

typedef struct {
    uint16_t frames;
    int sample_rate;
    int16_t pcm[OUTPUT_CHUNK_FRAMES];
} output_frame_t;

static voice_codec_t *s_codec;
static bool s_enabled;
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
static uint8_t *s_queue_storage;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static atomic_bool s_writing;

static void output_task(void *arg)
{
    (void)arg;
    output_frame_t frame;
    while (true) {
        if (xQueueReceive(s_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        atomic_store(&s_writing, true);
        esp_err_t err = voice_codec_write(s_codec, frame.pcm, frame.frames,
                                          frame.sample_rate);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec write failed: %s", esp_err_to_name(err));
        }
        atomic_store(&s_writing, false);
        if (uxQueueMessagesWaiting(s_queue) == 0) {
            xEventGroupSetBits(s_events, OUTPUT_DRAINED_BIT);
        }
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

    s_events = xEventGroupCreate();
    size_t bytes = OUTPUT_QUEUE_DEPTH * sizeof(output_frame_t);
#if CONFIG_IDF_TARGET_ESP32S3
    /* ~41 KB of jitter buffer; PSRAM keeps internal RAM for the AFE. */
    s_queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue_storage == NULL)
#endif
    s_queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_events == NULL || s_queue_storage == NULL) goto fail;
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

bool voice_output_active(void)
{
    if (!s_enabled) return false;
    return atomic_load(&s_writing) ||
           (s_queue != NULL && uxQueueMessagesWaiting(s_queue) > 0);
}

void voice_output_flush(void)
{
    if (!s_enabled || s_queue == NULL) return;
    xQueueReset(s_queue);
    xEventGroupSetBits(s_events, OUTPUT_DRAINED_BIT);
    ESP_LOGI(TAG, "playback flushed (barge-in)");
}

esp_err_t voice_output_write(const int16_t *pcm, size_t frames,
                             int sample_rate, int channels)
{
    if (!s_enabled) return ESP_OK;
    if (pcm == NULL || frames == 0 || sample_rate <= 0 || channels != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    xEventGroupClearBits(s_events, OUTPUT_DRAINED_BIT);
    while (frames > 0) {
        output_frame_t frame = {
            .frames = (uint16_t)(frames > OUTPUT_CHUNK_FRAMES ? OUTPUT_CHUNK_FRAMES : frames),
            .sample_rate = sample_rate,
        };
        memcpy(frame.pcm, pcm, frame.frames * sizeof(int16_t));
        /* Backpressure, not loss: the WebSocket task may wait while the codec
         * drains one frame. 20 ms was too tight for a whole phrase and made
         * the local wake ack fail on its 17th chunk. */
        if (xQueueSend(s_queue, &frame, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "playback queue stalled; dropping TTS tail");
            return ESP_ERR_TIMEOUT;
        }
        pcm += frame.frames;
        frames -= frame.frames;
    }
    return ESP_OK;
}

esp_err_t voice_output_wait_drained(unsigned timeout_ms)
{
    if (!s_enabled) return ESP_OK;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (uxQueueMessagesWaiting(s_queue) > 0 || atomic_load(&s_writing)) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= limit) return ESP_ERR_TIMEOUT;
        (void)xEventGroupWaitBits(s_events, OUTPUT_DRAINED_BIT, pdFALSE,
                                  pdFALSE, limit - elapsed);
    }
    return ESP_OK;
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
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    s_codec = NULL;
    s_enabled = false;
    atomic_store(&s_writing, false);
}
