#include "voice_state.h"

bool voice_state_transition_allowed(voice_state_t from, voice_state_t to)
{
    if (from < VOICE_STARTING || from > VOICE_DISABLED ||
        to < VOICE_STARTING || to > VOICE_DISABLED) return false;
    if (from == to) return true;
    switch (from) {
        case VOICE_STARTING: return to == VOICE_IDLE || to == VOICE_DISABLED;
        case VOICE_IDLE: return to == VOICE_CONNECTING;
        case VOICE_CONNECTING: return to == VOICE_LISTENING || to == VOICE_COOLDOWN;
        case VOICE_LISTENING: return to == VOICE_PROCESSING || to == VOICE_COOLDOWN;
        case VOICE_PROCESSING: return to == VOICE_COOLDOWN;
        case VOICE_COOLDOWN: return to == VOICE_IDLE;
        case VOICE_DISABLED: return false;
    }
    return false;
}

const char *voice_state_name(voice_state_t state)
{
    static const char *const names[] = {
        "starting", "idle", "connecting", "listening", "processing", "cooldown", "disabled"
    };
    return state >= VOICE_STARTING && state <= VOICE_DISABLED ? names[state] : "invalid";
}
