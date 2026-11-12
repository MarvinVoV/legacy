#include "voice_preroll.h"
#include <string.h>

void voice_preroll_reset(voice_preroll_t *s)
{
    s->head = s->used = 0;
    s->sequenced = false;
}

bool voice_preroll_push(voice_preroll_t *s, const int16_t *pcm,
                         size_t n, uint32_t sequence)
{
    if (!s->pcm || !s->capacity || !pcm || !n) return false;
    bool continuous = !s->sequenced || sequence == s->next_sequence;
    if (!continuous) voice_preroll_reset(s);
    s->sequenced = true;
    s->next_sequence = sequence + 1;
    if (n >= s->capacity) {
        pcm += n - s->capacity;
        n = s->capacity;
        s->head = s->used = 0;
    }
    if (s->used + n > s->capacity) {
        size_t drop = s->used + n - s->capacity;
        s->head = (s->head + drop) % s->capacity;
        s->used -= drop;
    }
    size_t tail = (s->head + s->used) % s->capacity;
    size_t first = s->capacity - tail;
    if (first > n) first = n;
    memcpy(s->pcm + tail, pcm, first * sizeof(*pcm));
    memcpy(s->pcm, pcm + first, (n - first) * sizeof(*pcm));
    s->used += n;
    return continuous;
}

size_t voice_preroll_peek(voice_preroll_t *s, const int16_t **pcm, size_t limit)
{
    *pcm = s->pcm ? s->pcm + s->head : NULL;
    size_t n = s->capacity - s->head;
    if (n > s->used) n = s->used;
    return n > limit ? limit : n;
}

void voice_preroll_consume(voice_preroll_t *s, size_t n)
{
    if (!s->capacity) return;
    if (n > s->used) n = s->used;
    s->head = (s->head + n) % s->capacity;
    s->used -= n;
}
