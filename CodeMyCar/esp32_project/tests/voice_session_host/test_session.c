#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "voice_protocol.h"
#include "voice_state.h"

static uint32_t message(voice_message_t *m, const char *text)
{
    return voice_message_feed(m, text, (int)strlen(text), 0, (int)strlen(text));
}

int main(void)
{
    voice_message_t a = {0}, b = {0};
    const char *ready = "{\"type\":\"ready\"}";
    int size = (int)strlen(ready);
    assert(!voice_message_feed(&a, ready, 4, 0, size));
    assert(message(&b, "{\"type\":\"end\"}") == VOICE_PEER_END);
    assert(voice_message_feed(&a, ready + 4, size - 4, 4, size) == VOICE_PEER_READY);
    assert(!voice_message_feed(&a, ready, 4, 0, size));
    assert(voice_message_feed(&a, ready + 6, size - 6, 6, size) == VOICE_PEER_FAILED);
    assert(voice_message_feed(&a, ready, size, 0, 2048) == VOICE_PEER_FAILED);
    assert(voice_message_feed(&a, ready, -1, 0, size) == VOICE_PEER_FAILED);
    assert(voice_message_feed(&a, ready, size, INT_MAX, size) == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":\"ready\"}garbage") == VOICE_PEER_FAILED);
    assert(message(&a, "[]") == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":42}") == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":\"recognizing\"}") == VOICE_PEER_RECOGNIZING);
    assert(message(&a, "{\"type\":\"error\"}") == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":\"final\"}") == VOICE_PEER_FINAL);
    assert(message(&a, "{\"type\":\"speak\"}") == VOICE_PEER_FAILED);
    assert(a.speak_rate == 0);
    assert(a.speak_channels == 0);
    assert(a.speak_bytes == 0);
    assert(message(&a, "{\"type\":\"speak\",\"text\":\"收到\",\"rate\":16000,"
                       "\"channels\":1,\"bytes\":640}") == VOICE_PEER_SPEAK);
    assert(a.speak_rate == 16000 && a.speak_channels == 1 && a.speak_bytes == 640);
    assert(message(&a, "{\"type\":\"speak\",\"rate\":0}") == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":\"speak\",\"channels\":3}") == VOICE_PEER_FAILED);
    assert(message(&a, "{\"type\":\"speak\",\"bytes\":-2}") == VOICE_PEER_FAILED);
    assert(message(&a, ready) == VOICE_PEER_READY);

    assert(message(&a, "{\"type\":\"playback_end\",\"turn\":2,\"seq\":3}") == VOICE_PEER_PLAYBACK_END);
    assert(a.turn == 2 && a.seq == 3);
    assert(message(&a, "{\"type\":\"playback_end\"}") == VOICE_PEER_FAILED);
    for (unsigned split = 1; split < 8; ++split) {
        const uint8_t raw[] = {1,0,2,0,255,255,0,128};
        voice_pcm_decoder_t d = {.remaining = sizeof(raw)};
        int16_t pcm[8]; size_t n, used=0;
        assert(voice_pcm_decode(&d,raw,split,pcm,8,&n)); used += n;
        assert(voice_pcm_decode(&d,raw+split,sizeof(raw)-split,pcm+used,8-used,&n)); used += n;
        assert(used == 4 && d.remaining == 0 && !d.have_carry);
        assert(pcm[0]==1 && pcm[1]==2 && pcm[2]==-1 && pcm[3]==-32768);
    }
    voice_sequence_t seq = {0};
    assert(voice_sequence_accept(&seq, UINT32_MAX));
    assert(voice_sequence_accept(&seq, 0));
    assert(!voice_sequence_accept(&seq, 2));
    assert(!voice_sequence_accept(&seq, 0));

    const voice_state_t path[] = {VOICE_STARTING, VOICE_IDLE, VOICE_CONNECTING,
        VOICE_LISTENING, VOICE_PROCESSING, VOICE_COOLDOWN, VOICE_IDLE};
    for (unsigned i = 1; i < sizeof(path) / sizeof(path[0]); ++i)
        assert(voice_state_transition_allowed(path[i-1], path[i]));
    assert(voice_state_transition_allowed(VOICE_CONNECTING, VOICE_COOLDOWN));
    assert(voice_state_transition_allowed(VOICE_LISTENING, VOICE_COOLDOWN));
    assert(voice_state_transition_allowed(VOICE_STARTING, VOICE_DISABLED));
    assert(!voice_state_transition_allowed(VOICE_DISABLED, VOICE_IDLE));
    assert(!voice_state_transition_allowed(VOICE_IDLE, VOICE_PROCESSING));
    assert(!voice_state_transition_allowed(VOICE_PROCESSING, VOICE_LISTENING));
    assert(!voice_state_transition_allowed((voice_state_t)99, VOICE_IDLE));
    assert(!strcmp(voice_state_name((voice_state_t)-1), "invalid"));
    puts("voice session: per-connection messages, fragmentation, malformed input, sequence gaps and legal states passed");
}
