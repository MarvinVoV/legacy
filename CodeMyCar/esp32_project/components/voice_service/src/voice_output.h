#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "voice_codec.h"

/* Board playback path. The server may stream TTS PCM after a `speak` header;
 * the codec decides whether this board can play it. A codec without output
 * makes write() a successful no-op so the control bridge and ASR stay alive. */
esp_err_t voice_output_init(voice_codec_t *codec);
bool voice_output_enabled(void);
esp_err_t voice_output_write(const int16_t *pcm, size_t frames,
                             int sample_rate, int channels);
esp_err_t voice_output_wait_drained(unsigned timeout_ms);
/* Drop everything queued (barge-in stop). The frame already inside the codec
 * finishes, so up to ~200 ms of DMA may still play. */
void voice_output_flush(void);
/* True while PCM is queued or being written to the codec. */
bool voice_output_active(void);
void voice_output_destroy(void);
