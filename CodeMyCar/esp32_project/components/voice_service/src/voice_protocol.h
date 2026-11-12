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
    VOICE_PEER_PLAYBACK_END = 1u << 9,
};

/* One parser per connection; no shared/global callback buffer. */
typedef struct {
    char text[2048];
    size_t used;
    size_t total;
    int speak_rate;
    int speak_channels;
    size_t speak_bytes;
    uint32_t turn, seq;
    bool barge_in, playback_ack, barge_audio;
} voice_message_t;

uint32_t voice_message_feed(voice_message_t *message, const char *data,
                            int size, int offset, int total);

/* RFC6455 message fragments and SDK chunks of one frame have different
 * boundaries. Text is bounded by message.text; binary stays streaming. */
typedef struct {
    size_t received, frame_size, text_size;
    uint8_t opcode, frame_opcode;
    bool active, in_frame, frame_fin;
} voice_ws_receiver_t;
uint32_t voice_ws_receive(voice_ws_receiver_t *receiver, voice_message_t *message,
                          uint8_t opcode, bool fin, const char *data,
                          int size, int offset, int total, bool *binary);

typedef struct { uint32_t next; bool initialized; } voice_sequence_t;
bool voice_sequence_accept(voice_sequence_t *sequence, uint32_t number);
void voice_sequence_sync(voice_sequence_t *sequence, uint32_t number);

typedef struct { size_t remaining; uint8_t carry; bool have_carry; } voice_pcm_decoder_t;
/* Consumes a byte slice, including a previous odd-byte carry. No partial
 * success on overflow: the caller must fail the stream instead of losing PCM. */
bool voice_pcm_decode(voice_pcm_decoder_t *decoder, const uint8_t *bytes, size_t len,
                       int16_t *pcm, size_t capacity, size_t *frames);
