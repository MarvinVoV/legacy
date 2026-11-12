#include "voice_transport.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "task_priorities.h"
#include "voice_output.h"

struct voice_transport {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    voice_message_t message;
    voice_ws_receiver_t receiver;
    voice_transport_audio_cb_t audio_cb;
    void *audio_arg;
    SemaphoreHandle_t guard;
    voice_pcm_decoder_t decoder;
    int speak_rate, speak_channels;
    atomic_bool speaking, barge_enabled, barge_audio;
    int64_t barge_started;
    int local_stop_ms;
    bool barge_tail_pending;
    uint32_t turn, seq, cancelled_turn, epoch;
    bool dropping, reply_active, ack_enabled, finish_pending, stop_pending;
    int16_t speak_pcm[1025];
};
static const char *TAG = "voice_transport";

static void feed_binary(voice_transport_t *t, const char *data, size_t len)
{
    xSemaphoreTake(t->guard, portMAX_DELAY);
    if (t->dropping) {
        t->decoder.remaining = len < t->decoder.remaining ? t->decoder.remaining - len : 0;
        xSemaphoreGive(t->guard);
        return;
    }
    size_t frames = 0;
    bool valid = voice_pcm_decode(&t->decoder, (const uint8_t *)data, len,
                                  t->speak_pcm, 1025, &frames);
    uint32_t epoch = t->epoch;
    int rate = t->speak_rate, channels = t->speak_channels;
    voice_transport_audio_cb_t cb = t->audio_cb;
    void *cb_arg = t->audio_arg;
    xSemaphoreGive(t->guard);
    if (!valid) { xEventGroupSetBits(t->events, VOICE_PEER_FAILED); return; }
    esp_err_t err = ESP_OK;
    if (frames && cb)
        err = cb(t->speak_pcm, frames, rate, channels, epoch, cb_arg);
    xSemaphoreTake(t->guard, portMAX_DELAY);
    if (err != ESP_OK && !t->dropping && epoch == t->epoch)
        xEventGroupSetBits(t->events, VOICE_PEER_FAILED);
    /* Keep a v2 reply active across sentence gaps, until playback_end + drain. */
    if (!t->ack_enabled && !t->decoder.remaining) atomic_store(&t->speaking, false);
    xSemaphoreGive(t->guard);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *payload)
{
    (void)base;
    voice_transport_t *t = arg;
    uint32_t events = 0;
    if (id == WEBSOCKET_EVENT_CONNECTED) events = VOICE_PEER_CONNECTED;
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        xEventGroupClearBits(t->events, VOICE_PEER_CONNECTED | VOICE_PEER_READY);
        events = id == WEBSOCKET_EVENT_DISCONNECTED ? VOICE_PEER_DISCONNECTED : VOICE_PEER_FAILED;
    } else if (id == WEBSOCKET_EVENT_DATA) {
        const esp_websocket_event_data_t *data = payload;
        bool binary = false;
        events = voice_ws_receive(&t->receiver, &t->message, data->op_code,
            data->fin, data->data_ptr, data->data_len,
            data->payload_offset, data->payload_len, &binary);
        if (events) {
            xSemaphoreTake(t->guard, portMAX_DELAY);
            if (events & VOICE_PEER_READY) {
                atomic_store(&t->barge_enabled, t->message.barge_in);
                t->ack_enabled = t->message.playback_ack;
                atomic_store(&t->barge_audio, t->message.barge_audio &&
                    t->message.barge_in && t->message.playback_ack);
            }
            if (events & VOICE_PEER_SPEAK) {
                bool invalid = (t->ack_enabled && (!t->message.turn || !t->message.seq)) ||
                    (t->decoder.remaining && !t->dropping) ||
                    (t->message.turn < t->turn) ||
                    (t->message.seq && t->message.seq <= t->seq);
                if (invalid) events |= VOICE_PEER_FAILED;
                else {
                    t->turn = t->message.turn;
                    t->seq = t->message.seq;
                    t->decoder = (voice_pcm_decoder_t){.remaining = t->message.speak_bytes};
                    t->speak_rate = t->message.speak_rate;
                    t->speak_channels = t->message.speak_channels;
                    t->dropping = t->turn && t->turn <= t->cancelled_turn;
                    t->epoch = voice_output_epoch();
                    if (!t->dropping) {
                        t->reply_active = true;
                        atomic_store(&t->speaking, true);
                    }
                }
            }
            if ((events & VOICE_PEER_PLAYBACK_END) && !t->dropping &&
                t->message.turn == t->turn && t->message.seq == t->seq) {
                if (t->decoder.remaining) events |= VOICE_PEER_FAILED;
                else t->finish_pending = true;
            }
            if ((events & VOICE_PEER_STOP) &&
                (t->message.turn == t->turn || (!t->ack_enabled && !t->message.turn))) {
                t->cancelled_turn = t->turn;
                t->dropping = true;
                t->reply_active = t->finish_pending = false;
                atomic_store(&t->speaking, false);
                voice_output_flush();
                t->stop_pending = t->ack_enabled;
            } else if (events & VOICE_PEER_STOP) {
                events &= ~VOICE_PEER_STOP; /* stale response: no mic reset either */
            }
            xSemaphoreGive(t->guard);
        } else if (binary) {
            feed_binary(t, data->data_ptr, (size_t)data->data_len);
        }
    }
    if (events) {
        xEventGroupSetBits(t->events, events);
        ESP_LOGI(TAG, "peer_event=0x%02lx", (unsigned long)events);
    }
}

void voice_transport_close(voice_transport_t *t)
{
    if (!t) return;
    if (t->client) {
        /* stop/destroy join callbacks before releasing their context/events. */
        esp_websocket_client_stop(t->client);
        esp_websocket_client_destroy(t->client);
    }
    voice_output_flush();
    if (t->guard) vSemaphoreDelete(t->guard);
    if (t->events) vEventGroupDelete(t->events);
    free(t);
}

static bool send_text(voice_transport_t *t, const char *text, unsigned timeout_ms)
{
    int n = (int)strlen(text);
    return esp_websocket_client_send_text(t->client, text, n, pdMS_TO_TICKS(timeout_ms)) == n;
}

esp_err_t voice_transport_open(voice_transport_t **out, bool continuous, bool one_shot,
                               voice_session_mode_t mode)
{
    *out = NULL;
    voice_transport_t *t = calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->events = xEventGroupCreate();
    if (!t->events) { voice_transport_close(t); return ESP_ERR_NO_MEM; }
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_CMCAR_VOICE_WS_URL,
        .disable_auto_reconnect = true,
        /* The audio callback returns between 20 ms frames, so the client
         * task is never blocked for long; 15 s detects a dead socket (server
         * restart) without tearing down a long TTS reply the way 5 s did. */
        .network_timeout_ms = 15000,
        .buffer_size = 2048,
#if CONFIG_IDF_TARGET_ESP32S3
        .task_stack = 6144,
#else
        .task_stack = 4096,
#endif
        .task_prio = TASK_PRIO_VOICE_NETWORK,
    };
    t->guard = xSemaphoreCreateMutex();
    if (!t->guard) { voice_transport_close(t); return ESP_ERR_NO_MEM; }
    t->client = esp_websocket_client_init(&cfg);
    if (!t->client) { voice_transport_close(t); return ESP_ERR_NO_MEM; }
    esp_err_t err = esp_websocket_register_events(t->client, WEBSOCKET_EVENT_ANY, on_event, t);
    if (err == ESP_OK) err = esp_websocket_client_start(t->client);
    if (err != ESP_OK) { voice_transport_close(t); return err; }
    EventBits_t bits = xEventGroupWaitBits(t->events, VOICE_PEER_CONNECTED | VOICE_PEER_FAILED | VOICE_PEER_DISCONNECTED,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & VOICE_PEER_CONNECTED) || (bits & VOICE_PEER_FAILED)) goto timeout;
    cJSON *start = cJSON_CreateObject();
    if (!start) { voice_transport_close(t); return ESP_ERR_NO_MEM; }
#if CONFIG_CMCAR_VOICE_AEC
    const bool barge_capable = true;
#else
    const bool barge_capable = false;
#endif
    bool complete = cJSON_AddStringToObject(start, "type", "start") &&
        cJSON_AddStringToObject(start, "device", CONFIG_CMCAR_DEVICE_ID) &&
        cJSON_AddNumberToObject(start, "rate", 16000) &&
        cJSON_AddNumberToObject(start, "protocol", 2) &&
        cJSON_AddBoolToObject(start, "playback_ack", voice_output_enabled()) &&
        cJSON_AddNumberToObject(start, "ch", 1) &&
        cJSON_AddNumberToObject(start, "bits", 16) &&
        cJSON_AddStringToObject(start, "mode",
            mode == VOICE_SESSION_CONVERSATION ? "conversation" : "command") &&
        cJSON_AddBoolToObject(start, "continuous", continuous) &&
        cJSON_AddBoolToObject(start, "one_shot", one_shot) &&
        /* The device already played its local "我在"; the server must not add
         * a second ready cue on top of it. */
        cJSON_AddBoolToObject(start, "local_ack", voice_output_enabled()) &&
        cJSON_AddBoolToObject(start, "barge_in", barge_capable) &&
        cJSON_AddBoolToObject(start, "barge_audio", barge_capable &&
            mode == VOICE_SESSION_CONVERSATION);
    char *text = complete ? cJSON_PrintUnformatted(start) : NULL;
    cJSON_Delete(start);
    bool sent = text && send_text(t, text, 1000);
    cJSON_free(text);
    if (!sent) { voice_transport_close(t); return ESP_FAIL; }
    bits = xEventGroupWaitBits(t->events, VOICE_PEER_READY | VOICE_PEER_FAILED | VOICE_PEER_DISCONNECTED,
                              pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & VOICE_PEER_READY) || (bits & VOICE_PEER_FAILED)) goto timeout;
    *out = t;
    return ESP_OK;
timeout:
    voice_transport_close(t);
    return ESP_ERR_TIMEOUT;
}

uint32_t voice_transport_events(voice_transport_t *t) { return xEventGroupGetBits(t->events); }

void voice_transport_clear_events(voice_transport_t *t, uint32_t mask)
{
    if (t != NULL) xEventGroupClearBits(t->events, mask);
}

void voice_transport_set_audio_callback(voice_transport_t *t,
                                        voice_transport_audio_cb_t cb, void *arg)
{
    if (!t) return;
    xSemaphoreTake(t->guard, portMAX_DELAY);
    t->audio_cb = cb;
    t->audio_arg = arg;
    xSemaphoreGive(t->guard);
}

esp_err_t voice_transport_send_pcm(voice_transport_t *t, const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0 || samples > 320) return ESP_ERR_INVALID_ARG;
    int bytes = (int)(samples * sizeof(*pcm));
    int sent = esp_websocket_client_send_bin(t->client, (const char *)pcm, bytes, pdMS_TO_TICKS(2000));
    return sent == bytes ? ESP_OK : ESP_FAIL;
}

bool voice_transport_is_speaking(const voice_transport_t *t)
{
    return t != NULL && atomic_load(&t->speaking);
}

bool voice_transport_barge_enabled(const voice_transport_t *t)
{
    return t && atomic_load(&t->barge_enabled);
}

bool voice_transport_barge_audio_enabled(const voice_transport_t *t)
{
    return t && atomic_load(&t->barge_audio);
}

esp_err_t voice_transport_barge_in(voice_transport_t *t)
{
    if (!voice_transport_barge_enabled(t)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(t->guard, portMAX_DELAY);
    if (!t->reply_active || t->dropping) {
        xSemaphoreGive(t->guard);
        return ESP_ERR_INVALID_STATE;
    }
    t->barge_started = esp_timer_get_time();
    t->barge_tail_pending = true;
    t->local_stop_ms = -1;
    t->cancelled_turn = t->turn;
    t->dropping = true;
    t->reply_active = t->finish_pending = false;
    atomic_store(&t->speaking, false);
    voice_output_flush();
    uint32_t turn = t->turn;
    xSemaphoreGive(t->guard);
    char message[96];
    snprintf(message, sizeof(message), "{\"type\":\"barge_in\",\"turn\":%lu}", (unsigned long)turn);
    return send_text(t, message, 500) ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_transport_poll(voice_transport_t *t)
{
    if (voice_output_failed()) return ESP_FAIL;
    char message[160] = {0};
    xSemaphoreTake(t->guard, portMAX_DELAY);
    if (!voice_output_active()) {
        if (t->barge_tail_pending) {
            t->local_stop_ms = (int)((esp_timer_get_time() - t->barge_started) / 1000);
            t->barge_tail_pending = false;
            ESP_LOGI(TAG, "barge local_dma_drain_ms=%d", t->local_stop_ms);
        }
        if (t->stop_pending) {
            t->stop_pending = false;
            snprintf(message, sizeof(message), "{\"type\":\"stop_ack\",\"turn\":%lu,\"seq\":%lu,\"local_dma_drain_ms\":%d}",
                     (unsigned long)t->turn, (unsigned long)t->seq, t->local_stop_ms);
        } else if (t->finish_pending) {
            t->finish_pending = t->reply_active = false;
            atomic_store(&t->speaking, false);
            snprintf(message, sizeof(message), "{\"type\":\"playback_finished\",\"turn\":%lu,\"seq\":%lu}",
                     (unsigned long)t->turn, (unsigned long)t->seq);
        }
    }
    xSemaphoreGive(t->guard);
    return !message[0] || send_text(t, message, 500) ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_transport_finish(voice_transport_t *t, unsigned timeout_ms)
{
    if (!send_text(t, "{\"type\":\"end\"}", 1000)) return ESP_FAIL;
    EventBits_t bits = xEventGroupWaitBits(t->events, VOICE_PEER_END | VOICE_PEER_FAILED | VOICE_PEER_DISCONNECTED,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & VOICE_PEER_FAILED) return ESP_FAIL;
    if (bits & VOICE_PEER_END) return ESP_OK;
    return (bits & VOICE_PEER_DISCONNECTED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void voice_transport_abort(voice_transport_t *t) { (void)send_text(t, "{\"type\":\"abort\"}", 500); }
