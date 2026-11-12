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
bool voice_output_failed(void);
uint32_t voice_output_epoch(void);
esp_err_t voice_output_write_epoch(const int16_t *pcm, size_t frames,
                                   int sample_rate, int channels, uint32_t epoch);
esp_err_t voice_output_write(const int16_t *pcm, size_t frames,
                             int sample_rate, int channels);
esp_err_t voice_output_wait_drained(unsigned timeout_ms);
/* Fence queued/dequeued producers. The existing hardware DMA tail is included
 * in active()/wait_drained(); RX/reference clocks remain running. */
void voice_output_flush(void);
/* Includes dequeued PCM and a conservative DMA tail deadline. */
bool voice_output_active(void);
void voice_output_destroy(void);
