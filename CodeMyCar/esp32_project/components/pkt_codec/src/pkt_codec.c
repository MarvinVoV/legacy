/**
 * @file pkt_codec.c
 * @brief UART 协议 v2 编解码器实现（零 malloc）
 */

#include "pkt_codec.h"

#include <string.h>

#include "cmcar_crc16.h"

/* ==================== 编码器 ==================== */

esp_err_t pkt_encode(const pkt_frame_t* frame, uint8_t* out_buf,
                     size_t out_buf_size, size_t* out_len)
{
    if (frame == NULL || out_buf == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->payload_len > PKT_MAX_PAYLOAD)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t total = (size_t)PKT_FRAME_OVERHEAD + frame->payload_len;
    if (out_buf_size < total)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* 填充帧头 */
    size_t pos = 0;
    out_buf[pos++] = PKT_MAGIC_LO;
    out_buf[pos++] = PKT_MAGIC_HI;
    out_buf[pos++] = frame->type;
    out_buf[pos++] = frame->seq;
    out_buf[pos++] = frame->flags;
    out_buf[pos++] = frame->payload_len;

    /* 填充 payload */
    if (frame->payload_len > 0)
    {
        memcpy(&out_buf[pos], frame->payload, frame->payload_len);
        pos += frame->payload_len;
    }

    /* 计算 CRC（覆盖 MAGIC 到 PAYLOAD） */
    const uint16_t crc = cmcar_crc16(out_buf, pos);
    out_buf[pos++] = (uint8_t)(crc & 0xFF);        /* CRC 低字节 */
    out_buf[pos++] = (uint8_t)((crc >> 8) & 0xFF); /* CRC 高字节 */

    *out_len = pos;
    return ESP_OK;
}

/* ==================== 解析器 ==================== */

void pkt_parser_init(pkt_parser_t* parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = PARSE_MAGIC_LO;
}

void pkt_parser_reset(pkt_parser_t* parser)
{
    parser->state = PARSE_MAGIC_LO;
    parser->header_idx = 0;
    parser->crc_running = 0;
}

bool pkt_parser_feed(pkt_parser_t* parser, uint8_t byte)
{
    switch (parser->state)
    {
        case PARSE_MAGIC_LO:
            if (byte == PKT_MAGIC_LO)
            {
                parser->crc_running = 0xFFFF; /* 重置 CRC */
                parser->crc_running = cmcar_crc16_continue(
                    parser->crc_running, &byte, 1);
                parser->state = PARSE_MAGIC_HI;
            }
            break;

        case PARSE_MAGIC_HI:
            if (byte == PKT_MAGIC_HI)
            {
                parser->crc_running = cmcar_crc16_continue(
                    parser->crc_running, &byte, 1);
                parser->header_idx = 0;
                parser->state = PARSE_HEADER;
            }
            else if (byte == PKT_MAGIC_LO)
            {
                /* 连续的 0x55，重新开始 CRC */
                parser->crc_running = 0xFFFF;
                parser->crc_running = cmcar_crc16_continue(
                    parser->crc_running, &byte, 1);
                /* 保持在 PARSE_MAGIC_HI */
            }
            else
            {
                parser->state = PARSE_MAGIC_LO;
            }
            break;

        case PARSE_HEADER:
            parser->header_buf[parser->header_idx++] = byte;
            parser->crc_running = cmcar_crc16_continue(
                parser->crc_running, &byte, 1);

            if (parser->header_idx >= 4)
            {
                parser->frame.type = parser->header_buf[0];
                parser->frame.seq = parser->header_buf[1];
                parser->frame.flags = parser->header_buf[2];
                parser->frame.payload_len = parser->header_buf[3];

                if (parser->frame.payload_len > PKT_MAX_PAYLOAD)
                {
                    /* 长度非法，丢弃 */
                    pkt_parser_reset(parser);
                }
                else if (parser->frame.payload_len == 0)
                {
                    parser->state = PARSE_CRC_LO;
                }
                else
                {
                    parser->header_idx = 0; /* 复用为 payload 索引 */
                    parser->state = PARSE_PAYLOAD;
                }
            }
            break;

        case PARSE_PAYLOAD:
            parser->frame.payload[parser->header_idx++] = byte;
            parser->crc_running = cmcar_crc16_continue(
                parser->crc_running, &byte, 1);

            if (parser->header_idx >= parser->frame.payload_len)
            {
                parser->state = PARSE_CRC_LO;
            }
            break;

        case PARSE_CRC_LO:
            parser->header_buf[0] = byte; /* 暂存 CRC 低字节 */
            parser->state = PARSE_CRC_HI;
            break;

        case PARSE_CRC_HI:
        {
            const uint16_t received_crc =
                (uint16_t)parser->header_buf[0] | ((uint16_t)byte << 8);

            if (received_crc == parser->crc_running)
            {
                /* 校验通过，帧完成 */
                parser->state = PARSE_MAGIC_LO;
                return true;
            }

            /* CRC 失败，丢弃 */
            pkt_parser_reset(parser);
            break;
        }
    }

    return false;
}

int pkt_parser_feed_buf(pkt_parser_t* parser, const uint8_t* data,
                        size_t len, pkt_frame_t* out_frames,
                        size_t max_frames)
{
    int count = 0;

    for (size_t i = 0; i < len && (size_t)count < max_frames; i++)
    {
        if (pkt_parser_feed(parser, data[i]))
        {
            if (out_frames != NULL)
            {
                memcpy(&out_frames[count], &parser->frame, sizeof(pkt_frame_t));
            }
            count++;
        }
    }

    return count;
}

/* ==================== ACK 管理器 ==================== */

void pkt_ack_mgr_init(pkt_ack_mgr_t* mgr)
{
    memset(mgr, 0, sizeof(*mgr));
}

uint8_t pkt_ack_mgr_next_seq(pkt_ack_mgr_t* mgr)
{
    return mgr->next_seq++;
}

esp_err_t pkt_ack_mgr_add(pkt_ack_mgr_t* mgr, uint8_t seq, uint32_t now_ms)
{
    for (int i = 0; i < PKT_MAX_PENDING_ACK; i++)
    {
        if (!mgr->slots[i].active)
        {
            mgr->slots[i].seq = seq;
            mgr->slots[i].retries = 0;
            mgr->slots[i].send_time_ms = now_ms;
            mgr->slots[i].active = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM; /* 所有槽位已满 */
}

bool pkt_ack_mgr_check(pkt_ack_mgr_t* mgr, uint8_t ack_seq)
{
    for (int i = 0; i < PKT_MAX_PENDING_ACK; i++)
    {
        if (mgr->slots[i].active && mgr->slots[i].seq == ack_seq)
        {
            mgr->slots[i].active = false;
            return true;
        }
    }
    return false;
}

int pkt_ack_mgr_check_timeout(pkt_ack_mgr_t* mgr, uint32_t now_ms,
                              uint8_t* expired_seqs, int max_expired)
{
    int count = 0;

    for (int i = 0; i < PKT_MAX_PENDING_ACK && count < max_expired; i++)
    {
        if (!mgr->slots[i].active)
        {
            continue;
        }

        const uint32_t elapsed = now_ms - mgr->slots[i].send_time_ms;
        if (elapsed >= PKT_ACK_TIMEOUT_MS)
        {
            mgr->slots[i].retries++;
            if (mgr->slots[i].retries >= PKT_ACK_MAX_RETRIES)
            {
                /* 超过最大重试次数，标记为过期 */
                if (expired_seqs != NULL)
                {
                    expired_seqs[count] = mgr->slots[i].seq;
                }
                count++;
                mgr->slots[i].active = false;
            }
            else
            {
                /* 重置计时，等待重发 */
                mgr->slots[i].send_time_ms = now_ms;
            }
        }
    }

    return count;
}
