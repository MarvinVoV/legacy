#pragma once
#include <stdbool.h>

typedef enum {
    VOICE_STARTING,
    VOICE_IDLE,
    VOICE_CONNECTING,
    VOICE_LISTENING,
    VOICE_PROCESSING, /* includes server-owned Mac playback */
    VOICE_COOLDOWN,
    VOICE_DISABLED,
} voice_state_t;

bool voice_state_transition_allowed(voice_state_t from, voice_state_t to);
const char *voice_state_name(voice_state_t state);
