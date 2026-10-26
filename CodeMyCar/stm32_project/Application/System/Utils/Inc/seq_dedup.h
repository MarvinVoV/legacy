#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 最近 N 个已处理 seq 的小窗口，用于目标命令去重 */
#define SEQ_DEDUP_SLOTS 4

typedef struct
{
    uint8_t seq[SEQ_DEDUP_SLOTS];
    uint8_t count;
    uint8_t next;
} seq_dedup_t;

void seq_dedup_init(seq_dedup_t* d);
bool seq_dedup_seen(const seq_dedup_t* d, uint8_t seq);
void seq_dedup_record(seq_dedup_t* d, uint8_t seq);
