#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ack_manager.h"

/* ACK_MGR_MAX_PENDING 在 ack_manager.h 默认 4 */

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); failures++; } else { printf("ok: %s\n", msg);} } while(0)

/* 记录 resend 调用次数 + 最近一帧 */
static int resend_count = 0;
static void fake_resend(const uint8_t* frame, size_t len, void* ctx)
{
    (void)frame; (void)len; (void)ctx;
    resend_count++;
}

int main(void)
{
    ack_manager_t m;
    ackmgr_init(&m);
    CHECK(m.next_seq == 0, "init next_seq=0");

    /* 1. alloc_seq 递增且不冲突（表空，连续 0,1,2,...） */
    uint8_t s0 = ackmgr_alloc_seq(&m);
    uint8_t s1 = ackmgr_alloc_seq(&m);
    CHECK(s0 == 0 && s1 == 1, "alloc_seq 0 then 1");

    /* 2. add 一帧 seq=5 @ t=0 */
    uint8_t frame[10] = {0xAA};
    CHECK(ackmgr_add(&m, 5, frame, 10, 0), "add seq=5");
    /* dup seq 拒绝 */
    CHECK(!ackmgr_add(&m, 5, frame, 10, 0), "dup seq=5 rejected");

    /* 3. scan @ t=50（未超时 200ms）→ 0 resend, 0 lost */
    resend_count = 0;
    uint32_t lost = ackmgr_scan(&m, 50, 200, 3, fake_resend, NULL);
    CHECK(lost == 0 && resend_count == 0, "scan before timeout: no resend no lost");

    /* 4. scan @ t=250（超时, retries 0→1）→ resend 1, 0 lost */
    lost = ackmgr_scan(&m, 250, 200, 3, fake_resend, NULL);
    CHECK(lost == 0 && resend_count == 1, "scan t250: resend once, no lost");

    /* 5. scan @ t=450（retries 1→2）→ resend */
    lost = ackmgr_scan(&m, 450, 200, 3, fake_resend, NULL);
    CHECK(lost == 0 && resend_count == 2, "scan t450: resend again");

    /* 6. scan @ t=650（retries 2→3 >= max 3）→ lost 1, slot inactive */
    lost = ackmgr_scan(&m, 650, 200, 3, fake_resend, NULL);
    CHECK(lost == 1, "scan t650: lost=1");
    /* 再扫：槽位已 inactive，不再重传/丢失 */
    resend_count = 0;
    lost = ackmgr_scan(&m, 900, 200, 3, fake_resend, NULL);
    CHECK(lost == 0 && resend_count == 0, "after lost: slot inactive, no action");

    /* 7. clear */
    ackmgr_add(&m, 9, frame, 10, 0);
    ackmgr_clear(&m, 9);
    /* clear 后同 seq 可再 add */
    CHECK(ackmgr_add(&m, 9, frame, 10, 0), "after clear, seq=9 re-addable");

    /* 8. 表满：add 满后再 add 新 seq 失败 */
    ackmgr_init(&m);
    bool ok = true;
    for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
    {
        ok = ok && ackmgr_add(&m, (uint8_t)(100 + i), frame, 10, 0);
    }
    CHECK(ok, "fill table to capacity");
    CHECK(!ackmgr_add(&m, 200, frame, 10, 0), "table full rejected");

    printf("\n%s (%d failures)\n", failures? "RESULT: FAIL":"RESULT: PASS", failures);
    return failures? 1:0;
}
