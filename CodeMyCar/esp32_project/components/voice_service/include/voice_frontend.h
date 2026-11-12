#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S3 only. One feed owner (capture) and one fetch/control owner. Never call
 * destroy while either task is running. Returned PCM expires on next fetch. */
bool voice_frontend_init(void);
bool voice_frontend_feed(const int16_t *pcm, size_t samples);
bool voice_frontend_fetch(const int16_t **pcm, size_t *samples,
                          bool *wake, bool *speech, bool allow_wake);
void voice_frontend_destroy(void);

/* AEC/double-talk diagnostics (valid when the reference channel is fed).
 * out_rms: AFE output level; echo_corr: best mic-vs-reference correlation
 * (0..1000) over the last chunk; ref_rms: playback reference level. */
int voice_frontend_out_rms(void);
int voice_frontend_echo_corr(void);
int voice_frontend_ref_rms(void);
