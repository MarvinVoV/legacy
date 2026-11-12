#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single session owner; storage is supplied by the caller (PSRAM on S3).
 * Discontinuities start a new window, never join unrelated audio. */
typedef struct {
    int16_t *pcm;
    size_t capacity, head, used;
    uint32_t next_sequence;
    bool sequenced;
} voice_preroll_t;
void voice_preroll_reset(voice_preroll_t *ring);
bool voice_preroll_push(voice_preroll_t *ring, const int16_t *pcm,
                         size_t samples, uint32_t sequence);
size_t voice_preroll_peek(voice_preroll_t *ring, const int16_t **pcm, size_t limit);
void voice_preroll_consume(voice_preroll_t *ring, size_t samples);
