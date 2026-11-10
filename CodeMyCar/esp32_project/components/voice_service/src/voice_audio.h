#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_codec.h"

#define VOICE_SAMPLE_RATE 16000
#define VOICE_CHUNK_FRAMES 320

typedef struct {
    uint32_t sequence;
    uint16_t frames;
    bool wake;
    int16_t pcm[VOICE_CHUNK_FRAMES];
} voice_frame_t;

/* Single hardware pipeline, one frame consumer (session owner).
 * init allocates only; start releases tasks only after both were created.
 * destroy is for startup rollback, never while the pipeline is running. */
esp_err_t voice_audio_init(void);
esp_err_t voice_audio_start(void);
void voice_audio_destroy(void);
bool voice_audio_receive(voice_frame_t *frame, unsigned timeout_ms);
void voice_audio_flush(void);
void voice_audio_set_active(bool active);
bool voice_audio_detect_wake(const voice_frame_t *frame);
bool voice_audio_take_energy_trigger(void);
void voice_audio_reset_wake(void);
voice_codec_t *voice_audio_codec(void);
