#include "voice_transport.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "task_priorities.h"

struct voice_transport {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    voice_message_t message;
    voice_transport_audio_cb_t audio_cb;
    void *audio_arg;
    size_t speak_remaining;
    int speak_rate;
    int speak_channels;
    uint8_t speak_carry;
    bool speak_have_carry;
    int16_t speak_pcm[1025];
};
static const char *TAG = "voice_transport";

static void feed_binary(voice_transport_t *t, const char *data, size_t len)
{
    if (t->speak_remaining == 0 || len == 0) return;
    if (len > t->speak_remaining) {
        ESP_LOGW(TAG, "speak frame exceeds declared bytes; truncating");
        len = t->speak_remaining;
    }
    const uint8_t *src = (const uint8_t *)data;
    size_t frames = 0;
    if (t->speak_have_carry) {
        t->speak_pcm[frames++] = (int16_t)(t->speak_carry | ((uint16_t)src[0] << 8));
        src++;
        len--;
        t->speak_have_carry = false;
    }
    size_t pairs = len / 2;
    if (pairs > 1024 - frames) pairs = 1024 - frames;
    for (size_t i = 0; i < pairs; ++i) {
        t->speak_pcm[frames++] = (int16_t)(src[2 * i] | ((uint16_t)src[2 * i + 1] << 8));
    }
    if ((len & 1u) && pairs * 2 < len) {
        t->speak_carry = src[pairs * 2];
        t->speak_have_carry = true;
    }
    t->speak_remaining -= len;
    if (frames > 0 && t->audio_cb != NULL) {
        esp_err_t err = t->audio_cb(t->speak_pcm, frames, t->speak_rate,
                                    t->speak_channels, t->audio_arg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speak sink rejected PCM: %s", esp_err_to_name(err));
        }
    }
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
        if (data->data_len <= 0) return;
        if (data->op_code == 1 || (data->op_code == 0 && t->message.total)) {
            events = voice_message_feed(&t->message, data->data_ptr,
                data->data_len, data->payload_offset, data->payload_len);
            if (events & VOICE_PEER_SPEAK) {
                t->speak_remaining = t->message.speak_bytes;
                t->speak_rate = t->message.speak_rate;
                t->speak_channels = t->message.speak_channels;
                t->speak_have_carry = false;
                t->speak_carry = 0;
            }
        } else if (data->op_code == 2 ||
                   (data->op_code == 0 && t->speak_remaining > 0)) {
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
    if (t->events) vEventGroupDelete(t->events);
    free(t);
}

static bool send_text(voice_transport_t *t, const char *text, unsigned timeout_ms)
{
    int n = (int)strlen(text);
    return esp_websocket_client_send_text(t->client, text, n, pdMS_TO_TICKS(timeout_ms)) == n;
}

esp_err_t voice_transport_open(voice_transport_t **out, bool continuous, bool one_shot)
{
    *out = NULL;
    voice_transport_t *t = calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->events = xEventGroupCreate();
    if (!t->events) { voice_transport_close(t); return ESP_ERR_NO_MEM; }
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_CMCAR_VOICE_WS_URL,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 5000,
        .buffer_size = 2048,
#if CONFIG_IDF_TARGET_ESP32S3
        .task_stack = 6144,
#else
        .task_stack = 4096,
#endif
        .task_prio = TASK_PRIO_VOICE_NETWORK,
    };
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
    bool complete = cJSON_AddStringToObject(start, "type", "start") &&
        cJSON_AddStringToObject(start, "device", CONFIG_CMCAR_DEVICE_ID) &&
        cJSON_AddNumberToObject(start, "rate", 16000) &&
        cJSON_AddNumberToObject(start, "ch", 1) &&
        cJSON_AddNumberToObject(start, "bits", 16) &&
        cJSON_AddBoolToObject(start, "continuous", continuous) &&
        cJSON_AddBoolToObject(start, "one_shot", one_shot);
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

void voice_transport_set_audio_callback(voice_transport_t *t,
                                        voice_transport_audio_cb_t cb, void *arg)
{
    if (!t) return;
    t->audio_cb = cb;
    t->audio_arg = arg;
}

esp_err_t voice_transport_send_pcm(voice_transport_t *t, const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0 || samples > 320) return ESP_ERR_INVALID_ARG;
    int bytes = (int)(samples * sizeof(*pcm));
    int sent = esp_websocket_client_send_bin(t->client, (const char *)pcm, bytes, pdMS_TO_TICKS(500));
    return sent == bytes ? ESP_OK : ESP_FAIL;
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
