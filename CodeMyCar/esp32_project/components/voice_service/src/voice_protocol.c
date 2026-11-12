#include "voice_protocol.h"
#include <string.h>
#include "cJSON.h"

static bool read_id(cJSON *root, const char *name, uint32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    *out = 0;
    if (!item) return true; /* legacy command sessions */
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0) ||
        item->valuedouble > UINT32_MAX) return false;
    *out = (uint32_t)item->valuedouble;
    return item->valuedouble == (double)*out;
}

static uint32_t parse_message(voice_message_t *m);

uint32_t voice_message_feed(voice_message_t *m, const char *data,
                            int size, int offset, int total)
{
    if (offset == 0) { m->used = 0; m->total = total > 0 ? (size_t)total : 0; }
    if (!data || size <= 0 || offset < 0 || total <= 0 ||
        (size_t)total >= sizeof(m->text) || (size_t)total != m->total ||
        (size_t)offset != m->used || size > total - offset) {
        m->used = m->total = 0;
        return VOICE_PEER_FAILED;
    }
    memcpy(m->text + m->used, data, (size_t)size);
    m->used += (size_t)size;
    if (m->used < m->total) return 0;
    return parse_message(m);
}

static uint32_t parse_message(voice_message_t *m)
{
    m->text[m->used] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(m->text, m->used + 1, NULL, true);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    uint32_t event = 0;
    m->speak_rate = 0;
    m->speak_channels = 0;
    m->speak_bytes = 0;
    bool ids_valid = read_id(root, "turn", &m->turn) && read_id(root, "seq", &m->seq);
    if (!cJSON_IsObject(root) || !cJSON_IsString(type)) event = VOICE_PEER_FAILED;
    else if (!ids_valid) event = VOICE_PEER_FAILED;
    else if (!strcmp(type->valuestring, "ready")) {
        m->barge_in = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "barge_in"));
        m->playback_ack = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playback_ack"));
        m->barge_audio = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "barge_audio"));
        event = VOICE_PEER_READY;
    }
    else if (!strcmp(type->valuestring, "final")) event = VOICE_PEER_FINAL;
    else if (!strcmp(type->valuestring, "recognizing")) event = VOICE_PEER_RECOGNIZING;
    else if (!strcmp(type->valuestring, "end")) event = VOICE_PEER_END;
    else if (!strcmp(type->valuestring, "stop")) event = VOICE_PEER_STOP;
    else if (!strcmp(type->valuestring, "playback_end"))
        event = m->turn && m->seq ? VOICE_PEER_PLAYBACK_END : VOICE_PEER_FAILED;
    else if (!strcmp(type->valuestring, "error")) event = VOICE_PEER_FAILED;
    else if (!strcmp(type->valuestring, "speak")) {
        cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "rate");
        cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
        cJSON *bytes = cJSON_GetObjectItemCaseSensitive(root, "bytes");
        cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
        const int r = cJSON_IsNumber(rate) ? rate->valueint : 16000;
        const int c = cJSON_IsNumber(channels) ? channels->valueint : 1;
        const double b = cJSON_IsNumber(bytes) ? bytes->valuedouble : 0.0;
        if ((rate && !cJSON_IsNumber(rate)) ||
            (channels && !cJSON_IsNumber(channels)) ||
            (bytes && !cJSON_IsNumber(bytes)) ||
            (format && (!cJSON_IsString(format) || strcmp(format->valuestring, "pcm"))) ||
            r != 16000 || c != 1 || !(b > 0.0) || b > 2.0 * 1024 * 1024 ||
            b != (double)(size_t)b || ((size_t)b % 2)) {
            event = VOICE_PEER_FAILED;
        } else {
            event = VOICE_PEER_SPEAK;
            m->speak_rate = r;
            m->speak_channels = c;
            m->speak_bytes = (size_t)b;
        }
    }
    /* Additional events are forward-compatible. */
    cJSON_Delete(root);
    m->used = m->total = 0;
    return event;
}

uint32_t voice_ws_receive(voice_ws_receiver_t *s, voice_message_t *m,
                          uint8_t opcode, bool fin, const char *data,
                          int size, int offset, int total, bool *binary)
{
    *binary = false;
    /* Control frames may interleave with a fragmented data message. */
    if (opcode == 8 || opcode == 9 || opcode == 10) return 0;
    if (size < 0 || offset < 0 || total < 0 || offset > total ||
        size > total - offset || (size && !data)) goto invalid;
    if (!s->in_frame) {
        if (offset != 0) goto invalid;
        if (opcode == 1 || opcode == 2) {
            if (s->active) goto invalid;
            s->active = true;
            s->opcode = opcode;
            s->text_size = 0;
        } else if (opcode != 0 || !s->active) goto invalid;
        s->frame_opcode = opcode;
        s->frame_fin = fin;
        s->frame_size = (size_t)total;
        s->received = 0;
        s->in_frame = true;
    }
    if (opcode != s->frame_opcode || fin != s->frame_fin ||
        (size_t)total != s->frame_size || (size_t)offset != s->received)
        goto invalid;
    if (s->opcode == 1) {
        if ((size_t)size >= sizeof(m->text) - s->text_size) goto invalid;
        if (size) memcpy(m->text + s->text_size, data, (size_t)size);
        s->text_size += (size_t)size;
    } else {
        *binary = size > 0;
    }
    s->received += (size_t)size;
    if (s->received == s->frame_size) {
        s->in_frame = false;
        if (fin) {
            s->active = false;
            if (s->opcode == 1) {
                m->used = s->text_size;
                return parse_message(m);
            }
        }
    }
    return 0;
invalid:
    *s = (voice_ws_receiver_t){0};
    m->used = m->total = 0;
    return VOICE_PEER_FAILED;
}

bool voice_sequence_accept(voice_sequence_t *s, uint32_t number)
{
    if (s->initialized && number != s->next) return false;
    s->initialized = true;
    s->next = number + 1; /* uint32 wraparound is intentional. */
    return true;
}

void voice_sequence_sync(voice_sequence_t *s, uint32_t number)
{
    s->initialized = true;
    s->next = number + 1;
}

bool voice_pcm_decode(voice_pcm_decoder_t *s, const uint8_t *data, size_t len,
                       int16_t *pcm, size_t capacity, size_t *frames)
{
    *frames = 0;
    if (!data || !pcm || len > s->remaining ||
        (len + (s->have_carry ? 1u : 0u)) / 2 > capacity) return false;
    const size_t consumed = len;
    if (s->have_carry && len) {
        pcm[(*frames)++] = (int16_t)(s->carry | ((uint16_t)*data++ << 8));
        --len;
        s->have_carry = false;
    }
    while (len >= 2) {
        pcm[(*frames)++] = (int16_t)(data[0] | ((uint16_t)data[1] << 8));
        data += 2; len -= 2;
    }
    if (len) { s->carry = *data; s->have_carry = true; }
    s->remaining -= consumed;
    return s->remaining != 0 || !s->have_carry;
}
