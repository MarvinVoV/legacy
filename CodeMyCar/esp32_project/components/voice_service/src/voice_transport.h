#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_protocol.h"

typedef struct voice_transport voice_transport_t;
esp_err_t voice_transport_open(voice_transport_t **out, bool continuous, bool one_shot);
uint32_t voice_transport_events(voice_transport_t *transport);
esp_err_t voice_transport_send_pcm(voice_transport_t *transport, const int16_t *pcm, size_t samples);
esp_err_t voice_transport_finish(voice_transport_t *transport, unsigned timeout_ms);
void voice_transport_abort(voice_transport_t *transport);
void voice_transport_close(voice_transport_t *transport);
