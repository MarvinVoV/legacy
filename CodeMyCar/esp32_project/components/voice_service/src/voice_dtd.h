#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One owner, one call per fresh AFE output window. All decisions use that
 * window's residual PCM and VAD, never asynchronous raw input diagnostics. */
typedef struct {
    uint32_t warmup, held, gap, cooldown;
    bool playing;
} voice_dtd_t;
bool voice_dtd_update(voice_dtd_t *state, bool playing, bool speech,
                       int residual_ac_rms, size_t samples);
