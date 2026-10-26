#include "seq_dedup.h"

#include <string.h>

void seq_dedup_init(seq_dedup_t* d)
{
    if (d != NULL)
    {
        memset(d, 0, sizeof(*d));
    }
}

bool seq_dedup_seen(const seq_dedup_t* d, uint8_t seq)
{
    if (d == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < d->count; i++)
    {
        if (d->seq[i] == seq)
        {
            return true;
        }
    }
    return false;
}

void seq_dedup_record(seq_dedup_t* d, uint8_t seq)
{
    if (d == NULL || seq_dedup_seen(d, seq))
    {
        return;
    }
    if (d->count < SEQ_DEDUP_SLOTS)
    {
        d->seq[d->count++] = seq;
        return;
    }
    d->seq[d->next] = seq;
    d->next = (uint8_t)((d->next + 1u) % SEQ_DEDUP_SLOTS);
}
