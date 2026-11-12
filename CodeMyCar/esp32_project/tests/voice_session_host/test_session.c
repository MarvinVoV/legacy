#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "voice_protocol.h"
#include "voice_state.h"
#include "voice_preroll.h"

static uint32_t message(voice_message_t *m, const char *text)
{
    return voice_message_feed(m, text, (int)strlen(text), 0, (int)strlen(text));
}

static void websocket_fragments(void)
{
    const char *json = "{\"type\":\"ready\",\"barge_in\":true}";
    const int length = (int)strlen(json);
    for (int split = 1; split < length; ++split) {
        voice_ws_receiver_t ws = {0}; voice_message_t m = {0}; bool binary;
        /* First WS fragment delivered in two SDK callbacks. */
        int chunk = split / 2;
        assert(!voice_ws_receive(&ws,&m,1,false,json,chunk,0,split,&binary));
        assert(!binary);
        assert(!voice_ws_receive(&ws,&m,1,false,json+chunk,split-chunk,chunk,split,&binary));
        assert(!voice_ws_receive(&ws,&m,9,true,"ping",4,0,4,&binary));
        assert(voice_ws_receive(&ws,&m,0,true,json+split,length-split,0,length-split,&binary)==VOICE_PEER_READY);
        assert(!binary && !ws.active && m.barge_in);
    }
    voice_ws_receiver_t ws = {0}; voice_message_t m = {0}; bool binary;
    assert(!voice_ws_receive(&ws,&m,1,false,json,length,0,length,&binary));
    /* Empty final continuation must release a complete text message. */
    assert(voice_ws_receive(&ws,&m,0,true,NULL,0,0,0,&binary)==VOICE_PEER_READY);
    assert(!voice_ws_receive(&ws,&m,2,false,"a",1,0,1,&binary) && binary);
    assert(!voice_ws_receive(&ws,&m,0,true,"b",1,0,1,&binary) && binary);
    assert(voice_ws_receive(&ws,&m,0,true,"orphan",6,0,6,&binary)==VOICE_PEER_FAILED);
    assert(!voice_ws_receive(&ws,&m,1,false,json,3,0,3,&binary));
    assert(voice_ws_receive(&ws,&m,2,true,"a",1,0,1,&binary)==VOICE_PEER_FAILED);
    assert(!voice_ws_receive(&ws,&m,1,true,json,2,0,length,&binary));
    assert(voice_ws_receive(&ws,&m,1,true,json+3,length-3,3,length,&binary)==VOICE_PEER_FAILED);
    char large[2048]; memset(large,'x',sizeof(large));
    assert(!voice_ws_receive(&ws,&m,1,false,large,1024,0,1024,&binary));
    assert(voice_ws_receive(&ws,&m,0,true,large,1024,0,1024,&binary)==VOICE_PEER_FAILED);
    assert(voice_ws_receive(&ws,&m,1,true,json,length,0,length,&binary)==VOICE_PEER_READY);
}

static void preroll_windows(void)
{
    int16_t storage[8], input[12], result[8];
    for (int i=0; i<12; ++i) input[i]=(int16_t)i;
    voice_preroll_t s = {.pcm=storage,.capacity=8};
    assert(voice_preroll_push(&s,input,5,UINT32_MAX));
    assert(voice_preroll_push(&s,input+5,5,0));
    assert(s.used==8);
    size_t used=0;
    while (s.used) {
        const int16_t *p; size_t n=voice_preroll_peek(&s,&p,3);
        memcpy(result+used,p,n*sizeof(*p)); used+=n;
        voice_preroll_consume(&s,n);
    }
    for (int i=0;i<8;++i) assert(result[i]==i+2);
    assert(!voice_preroll_push(&s,input,2,10)); /* gap resets sequence/window */
    assert(s.used==2);
    assert(voice_preroll_push(&s,input,12,11));
    assert(s.used==8);
    const int16_t *p; assert(voice_preroll_peek(&s,&p,8)==8 && p[0]==4 && p[7]==11);
    voice_preroll_reset(&s); assert(s.used==0 && !s.sequenced);
    s.pcm=NULL; assert(!voice_preroll_push(&s,input,1,0));
}

int main(void)
{
    websocket_fragments();
    preroll_windows();
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
