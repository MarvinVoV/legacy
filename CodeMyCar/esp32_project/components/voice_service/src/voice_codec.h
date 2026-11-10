#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Board audio boundary.
 *
 * The voice pipeline must not know whether PCM comes from the legacy ICS43434
 * I2S microphone or from a future codec board (ES7210/ES8311/analog amp).
 * A codec implementation owns the bus, slot selection, gain/volume and, when
 * present, the playback reference channel.
 *
 * read() always delivers 16 kHz mono frames to the caller; a board codec that
 * captures at another rate/channel count converts internally. output_channels
 * is 0 when the board has no playback path yet.
 */
typedef struct {
    int input_sample_rate;
    int output_sample_rate;
    int input_channels;
    int output_channels;
    bool input_reference;
} voice_codec_caps_t;

typedef struct voice_codec voice_codec_t;

typedef struct {
    /* Fill exactly `frames` mono samples; return an error on short/timeout. */
    esp_err_t (*read)(voice_codec_t *codec, int16_t *dst, size_t frames);
    /* Play `frames` mono samples at `sample_rate`; optional. */
    esp_err_t (*write)(voice_codec_t *codec, const int16_t *src, size_t frames,
                       int sample_rate);
    void (*set_volume)(voice_codec_t *codec, int volume);
    void (*destroy)(voice_codec_t *codec);
} voice_codec_ops_t;

struct voice_codec {
    const voice_codec_ops_t *ops;
    voice_codec_caps_t caps;
    void *impl;
};

/* Select the board codec: legacy I2S mic by default, or a board-specific
 * implementation when CMCAR_VOICE_CODEC_BOARD is enabled. */
esp_err_t voice_codec_init(voice_codec_t **out);
void voice_codec_destroy(voice_codec_t *codec);
bool voice_codec_has_output(const voice_codec_t *codec);

/* Thin dispatch helpers so callers never touch ops directly. */
esp_err_t voice_codec_read(voice_codec_t *codec, int16_t *dst, size_t frames);
esp_err_t voice_codec_write(voice_codec_t *codec, const int16_t *src,
                            size_t frames, int sample_rate);
void voice_codec_set_volume(voice_codec_t *codec, int volume);

/* Implemented by the selected board profile. */
esp_err_t voice_codec_i2s_init(voice_codec_t **out);
esp_err_t voice_codec_board_init(voice_codec_t **out);
