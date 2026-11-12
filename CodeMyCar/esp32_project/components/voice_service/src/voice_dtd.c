#include "voice_dtd.h"

bool voice_dtd_update(voice_dtd_t *s, bool playing, bool speech, int rms, size_t n)
{
    /* 16kHz. A stream gap must not join unrelated pieces of speech. */
    if (!n || n > 1600) { s->held = s->gap = 0; return false; }
    s->cooldown = s->cooldown > n ? s->cooldown - n : 0;
    if (!playing) { s->playing = false; s->held = s->gap = 0; return false; }
    if (!s->playing) {
        s->playing = true; s->warmup = 4800; s->held = s->gap = 0;
    }
    if (s->warmup) {
        s->warmup = s->warmup > n ? s->warmup - n : 0;
        return false;
    }
    if (s->cooldown) { s->held = s->gap = 0; return false; }
    if (speech && rms >= 300) { s->held += n; s->gap = 0; }
    else {
        s->gap += n;
        if (s->gap > 1024) s->held = 0; /* <=64ms phoneme gap */
    }
    if (s->held < 3200) return false;
    s->held = s->gap = 0;
    s->cooldown = 40000; /* 2.5s, measured in actual processed samples */
    return true;
}
