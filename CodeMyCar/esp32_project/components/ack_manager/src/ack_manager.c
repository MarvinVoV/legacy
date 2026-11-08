/**
 * @file ack_manager.c
 * @brief ACK 重传表实现（纯 C，见 ack_manager.h）
 */

#include "ack_manager.h"

#include <string.h>

void ackmgr_init(ack_manager_t* m)
{
    if (m == NULL)
    {
        return;
    }
    memset(m, 0, sizeof(*m));
}

uint8_t ackmgr_alloc_seq(ack_manager_t* m)
{
    if (m == NULL)
    {
        return 0;
    }
    /* 跳过当前在途的 seq，最多尝试 256 次（全覆盖） */
    for (int i = 0; i < 256; i++)
    {
        const uint8_t seq = m->next_seq;
        m->next_seq++;
        bool in_flight = false;
        for (int j = 0; j < ACK_MGR_MAX_PENDING; j++)
        {
            if (m->slots[j].active && m->slots[j].seq == seq)
            {
                in_flight = true;
                break;
            }
        }
        if (!in_flight)
        {
            return seq;
        }
    }
    /* 全部 256 个 seq 都在途（不可能，深度=4）——回退给一个值 */
    return m->next_seq++;
}

bool ackmgr_is_free(const ack_manager_t* m, uint8_t seq)
{
    if (m == NULL)
    {
        return false;
    }
    for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
    {
        if (m->slots[i].active && m->slots[i].seq == seq)
        {
            return false;
        }
    }
    return true;
}

void ackmgr_cancel_motion(ack_manager_t* m)
{
    if (m == NULL) { return; }
    for (unsigned i = 0; i < ACK_MGR_MAX_PENDING; ++i)
    {
        if (!m->slots[i].priority) { m->slots[i].active = false; }
    }
}

static bool add_internal(ack_manager_t* m, uint8_t seq, uint8_t client_seq,
                         const uint8_t* frame, size_t len, uint32_t now_ms,
                         bool priority)
{
    if (m == NULL || frame == NULL || len > PKT_MAX_FRAME_SIZE)
    {
        return false;
    }
    int target = -1;
    for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
    {
        if (m->slots[i].active && m->slots[i].seq == seq)
        {
            if (!priority || m->slots[i].priority)
            {
                return false;
            }
            target = i;
            break;
        }
    }
    for (int i = 0; target < 0 && i < ACK_MGR_MAX_PENDING; i++)
    {
        if (!m->slots[i].active)
        {
            target = i;
        }
    }
    if (target < 0 && priority)
    {
        for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
        {
            if (!m->slots[i].priority &&
                (target < 0 ||
                 (uint32_t)(now_ms - m->slots[i].send_time_ms) >
                 (uint32_t)(now_ms - m->slots[target].send_time_ms)))
            {
                target = i;
            }
        }
    }
    if (target < 0)
    {
        return false;
    }
    m->slots[target].active = true;
    m->slots[target].seq = seq;
    m->slots[target].client_seq = client_seq;
    memcpy(m->slots[target].frame, frame, len);
    m->slots[target].frame_len = len;
    m->slots[target].send_time_ms = now_ms;
    m->slots[target].retries = 0;
    m->slots[target].priority = priority;
    return true;
}

bool ackmgr_add(ack_manager_t* m, uint8_t seq, const uint8_t* frame,
                size_t len, uint32_t now_ms)
{
    return add_internal(m, seq, seq, frame, len, now_ms, false);
}

bool ackmgr_add_priority(ack_manager_t* m, uint8_t seq, const uint8_t* frame,
                         size_t len, uint32_t now_ms)
{
    return add_internal(m, seq, seq, frame, len, now_ms, true);
}

bool ackmgr_add_mapped(ack_manager_t* m, uint8_t wire_seq, uint8_t client_seq,
                       const uint8_t* frame, size_t len, uint32_t now_ms,
                       bool priority)
{
    return add_internal(m, wire_seq, client_seq, frame, len, now_ms, priority);
}

bool ackmgr_clear_mapped(ack_manager_t* m, uint8_t seq,
                         uint8_t* out_client_seq)
{
    if (m == NULL)
    {
        return false;
    }
    for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
    {
        if (m->slots[i].active && m->slots[i].seq == seq)
        {
            if (out_client_seq != NULL)
            {
                *out_client_seq = m->slots[i].client_seq;
            }
            m->slots[i].active = false;
            return true;
        }
    }
    return false;
}

bool ackmgr_clear(ack_manager_t* m, uint8_t seq)
{
    return ackmgr_clear_mapped(m, seq, NULL);
}

uint32_t ackmgr_scan(ack_manager_t* m, uint32_t now_ms, uint32_t timeout_ms,
                      uint8_t max_retries, ackmgr_resend_fn resend, void* ctx)
{
    if (m == NULL)
    {
        return 0;
    }
    uint32_t lost = 0;
    for (int i = 0; i < ACK_MGR_MAX_PENDING; i++)
    {
        if (!m->slots[i].active)
        {
            continue;
        }
        if (now_ms - m->slots[i].send_time_ms < timeout_ms)
        {
            continue;
        }
        m->slots[i].retries++;
        if (m->slots[i].retries >= max_retries)
        {
            m->slots[i].active = false;
            lost++;
        }
        else if (resend != NULL)
        {
            resend(m->slots[i].frame, m->slots[i].frame_len, m->slots[i].priority, ctx);
            m->slots[i].send_time_ms = now_ms;
        }
    }
    return lost;
}
