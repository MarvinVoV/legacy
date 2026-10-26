#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "seq_dedup.h"

int main(void)
{
    seq_dedup_t d;
    seq_dedup_init(&d);

    seq_dedup_record(&d, 1);
    seq_dedup_record(&d, 2);
    seq_dedup_record(&d, 3);
    seq_dedup_record(&d, 4);

    assert(seq_dedup_seen(&d, 1));
    assert(seq_dedup_seen(&d, 3));
    assert(!seq_dedup_seen(&d, 5));

    /* 第 5 个覆盖最旧的 1 */
    seq_dedup_record(&d, 5);
    assert(!seq_dedup_seen(&d, 1));
    assert(seq_dedup_seen(&d, 2));
    assert(seq_dedup_seen(&d, 5));

    /* 重复记录不应破坏窗口 */
    seq_dedup_record(&d, 5);
    assert(seq_dedup_seen(&d, 5));
    assert(seq_dedup_seen(&d, 2));

    printf("seq_dedup: all tests passed\n");
    return 0;
}
