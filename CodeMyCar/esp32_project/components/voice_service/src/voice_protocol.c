#include "voice_protocol.h"
#include <string.h>
#include "cJSON.h"

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
    m->text[m->used] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(m->text, m->used + 1, NULL, true);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    uint32_t event = 0;
    if (!cJSON_IsObject(root) || !cJSON_IsString(type)) event = VOICE_PEER_FAILED;
    else if (!strcmp(type->valuestring, "ready")) event = VOICE_PEER_READY;
    else if (!strcmp(type->valuestring, "final")) event = VOICE_PEER_FINAL;
    else if (!strcmp(type->valuestring, "recognizing")) event = VOICE_PEER_RECOGNIZING;
    else if (!strcmp(type->valuestring, "end")) event = VOICE_PEER_END;
    else if (!strcmp(type->valuestring, "error")) event = VOICE_PEER_FAILED;
    /* Additional events such as speak/action are forward-compatible. */
    cJSON_Delete(root);
    m->used = m->total = 0;
    return event;
}

bool voice_sequence_accept(voice_sequence_t *s, uint32_t number)
{
    if (s->initialized && number != s->next) return false;
    s->initialized = true;
    s->next = number + 1; /* uint32 wraparound is intentional. */
    return true;
}
