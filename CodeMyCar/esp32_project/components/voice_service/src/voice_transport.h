#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_protocol.h"

typedef struct voice_transport voice_transport_t;

/* Called from the WebSocket task for server TTS PCM. Return quickly; the
 * output layer owns buffering and drops on overload. */
typedef esp_err_t (*voice_transport_audio_cb_t)(const int16_t *pcm, size_t frames,
                                                int sample_rate, int channels,
                                                void *arg);

esp_err_t voice_transport_open(voice_transport_t **out, bool continuous, bool one_shot);
void voice_transport_set_audio_callback(voice_transport_t *transport,
                                        voice_transport_audio_cb_t cb, void *arg);
uint32_t voice_transport_events(voice_transport_t *transport);
esp_err_t voice_transport_send_pcm(voice_transport_t *transport, const int16_t *pcm, size_t samples);
esp_err_t voice_transport_finish(voice_transport_t *transport, unsigned timeout_ms);
void voice_transport_abort(voice_transport_t *transport);
void voice_transport_close(voice_transport_t *transport);
