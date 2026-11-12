#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S3 only. One feed owner (capture) and one fetch/control owner. Never call
 * destroy while either task is running. Returned PCM expires on next fetch. */
bool voice_frontend_init(void);
bool voice_frontend_feed(const int16_t *pcm, size_t samples);
/* Feed owner only: abandon a partial input window and signal the fetch owner
 * to publish a continuity break and restart detector convergence. */
void voice_frontend_discontinuity(void);
bool voice_frontend_fetch(const int16_t **pcm, size_t *samples,
                          bool *wake, bool *speech, bool allow_wake);
void voice_frontend_destroy(void);
void voice_frontend_set_playback(bool enabled);
bool voice_frontend_take_barge_in(void);

/* Diagnostics only: residual AC level and raw reference have different
 * pipeline delays. Never combine these getters into a DTD observation. */
int voice_frontend_out_rms(void);
int voice_frontend_ref_rms(void);
