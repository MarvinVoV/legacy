#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VOICE_PEER_CONNECTED = 1u << 0,
    VOICE_PEER_READY = 1u << 1,
    VOICE_PEER_END = 1u << 2,
    VOICE_PEER_FAILED = 1u << 3,
    VOICE_PEER_FINAL = 1u << 4,
    VOICE_PEER_RECOGNIZING = 1u << 5,
    VOICE_PEER_DISCONNECTED = 1u << 6,
    VOICE_PEER_SPEAK = 1u << 7,
    VOICE_PEER_STOP = 1u << 8,
};

/* One parser per connection; no shared/global callback buffer. */
typedef struct {
    char text[2048];
    size_t used;
    size_t total;
    int speak_rate;
    int speak_channels;
    size_t speak_bytes;
} voice_message_t;

uint32_t voice_message_feed(voice_message_t *message, const char *data,
                            int size, int offset, int total);

typedef struct { uint32_t next; bool initialized; } voice_sequence_t;
bool voice_sequence_accept(voice_sequence_t *sequence, uint32_t number);
void voice_sequence_sync(voice_sequence_t *sequence, uint32_t number);
