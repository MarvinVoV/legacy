#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_protocol.h"

typedef struct voice_transport voice_transport_t;

typedef enum {
    VOICE_SESSION_COMMAND = 0,
    VOICE_SESSION_CONVERSATION = 1,
} voice_session_mode_t;

/* Called from the WebSocket task for server TTS PCM. Return quickly; the
 * output layer owns bounded buffering; overload fails the reply. */
typedef esp_err_t (*voice_transport_audio_cb_t)(const int16_t *pcm, size_t frames,
                                                int sample_rate, int channels,
                                                uint32_t epoch, void *arg);

esp_err_t voice_transport_open(voice_transport_t **out, bool continuous, bool one_shot,
                               voice_session_mode_t mode);
void voice_transport_set_audio_callback(voice_transport_t *transport,
                                        voice_transport_audio_cb_t cb, void *arg);
uint32_t voice_transport_events(voice_transport_t *transport);
void voice_transport_clear_events(voice_transport_t *transport, uint32_t mask);
esp_err_t voice_transport_send_pcm(voice_transport_t *transport, const int16_t *pcm, size_t samples);
/* True while TTS PCM is still arriving/being played. The session loop drops
 * microphone frames during this window (half-duplex, no self-echo). */
bool voice_transport_is_speaking(const voice_transport_t *transport);
bool voice_transport_barge_enabled(const voice_transport_t *transport);
esp_err_t voice_transport_poll(voice_transport_t *transport);
/* Device-side barge-in: discard remaining TTS PCM and tell the server to
 * cancel the current turn. Only a later response ID may resume playback. */
esp_err_t voice_transport_barge_in(voice_transport_t *transport);
esp_err_t voice_transport_finish(voice_transport_t *transport, unsigned timeout_ms);
void voice_transport_abort(voice_transport_t *transport);
void voice_transport_close(voice_transport_t *transport);
