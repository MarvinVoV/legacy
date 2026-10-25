/*
 * pkt_protocol.c
 *
 * UART 协议 v2 编解码器实现（与 esp32/components/pkt_codec 单一事实源一致）
 */

#include "pkt_protocol.h"

#include <string.h>
#include <stdio.h>

/* ==================== CRC16-CCITT ==================== */

uint16_t crc16_ccitt(const uint8_t* data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    return crc16_ccitt_continue(crc, data, length);
}

uint16_t crc16_ccitt_continue(uint16_t crc, const uint8_t* data, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ==================== 编码器 ==================== */

int protocol_encode(const protocol_frame_t* frame, uint8_t* out_buf,
                    size_t out_buf_size, size_t* out_len)
{
    if (frame == NULL || out_buf == NULL || out_len == NULL)
    {
        return -1;
    }
    if (frame->payload_len > PROTOCOL_MAX_PAYLOAD)
    {
        return -2;
    }

    const size_t total = (size_t)PROTOCOL_FRAME_OVERHEAD + frame->payload_len;
    if (out_buf_size < total)
    {
        return -2;
    }

    size_t pos = 0;
    out_buf[pos++] = FRAME_MAGIC_LO;
    out_buf[pos++] = FRAME_MAGIC_HI;
    out_buf[pos++] = frame->type;
    out_buf[pos++] = frame->seq;
    out_buf[pos++] = frame->flags;
    out_buf[pos++] = frame->payload_len;

    if (frame->payload_len > 0)
    {
        memcpy(&out_buf[pos], frame->payload, frame->payload_len);
        pos += frame->payload_len;
    }

    const uint16_t crc = crc16_ccitt(out_buf, (uint16_t)pos);
    out_buf[pos++] = (uint8_t)(crc & 0xFF);
    out_buf[pos++] = (uint8_t)((crc >> 8) & 0xFF);

    *out_len = pos;
    return 0;
}

uint8_t* protocol_pack_frame_ex(protocol_type_t type, uint8_t seq, uint8_t flags,
                                const uint8_t* data, uint16_t data_len,
                                uint16_t* frame_len)
{
    if (frame_len == NULL || (data == NULL && data_len > 0))
    {
        return NULL;
    }
    if (data_len > PROTOCOL_MAX_PAYLOAD)
    {
        return NULL;
    }

    protocol_frame_t frame = {
        .type = (uint8_t)type,
        .seq = seq,
        .flags = flags,
        .payload_len = (uint8_t)data_len,
    };
    if (data_len > 0)
    {
        memcpy(frame.payload, data, data_len);
    }

    size_t out_len = 0;
    uint8_t* buf = (uint8_t*)malloc(PROTOCOL_MAX_FRAME_SIZE);
    if (buf == NULL)
    {
        return NULL;
    }
    if (protocol_encode(&frame, buf, PROTOCOL_MAX_FRAME_SIZE, &out_len) != 0)
    {
        free(buf);
        return NULL;
    }
    *frame_len = (uint16_t)out_len;
    return buf;
}

uint8_t* protocol_pack_frame(protocol_type_t type, const uint8_t* data,
                             uint16_t data_len, uint16_t* frame_len)
{
    return protocol_pack_frame_ex(type, 0, 0, data, data_len, frame_len);
}

/* ==================== 解析器 ==================== */

void protocol_parser_init(protocol_parser_t* parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = PARSE_MAGIC_LO;
}

void protocol_parser_reset(protocol_parser_t* parser)
{
    parser->state = PARSE_MAGIC_LO;
    parser->header_idx = 0;
    parser->crc_running = 0;
}

int protocol_parse_byte(protocol_parser_t* parser, uint8_t byte)
{
    switch (parser->state)
    {
        case PARSE_MAGIC_LO:
            if (byte == FRAME_MAGIC_LO)
            {
                parser->crc_running = crc16_ccitt_continue(0xFFFF, &byte, 1);
                parser->state = PARSE_MAGIC_HI;
            }
            break;

        case PARSE_MAGIC_HI:
            if (byte == FRAME_MAGIC_HI)
            {
                parser->crc_running = crc16_ccitt_continue(parser->crc_running, &byte, 1);
                parser->header_idx = 0;
                parser->state = PARSE_HEADER;
            }
            else if (byte == FRAME_MAGIC_LO)
            {
                /* 连续 0x55：重新起算 CRC，保持 PARSE_MAGIC_HI */
                parser->crc_running = crc16_ccitt_continue(0xFFFF, &byte, 1);
            }
            else
            {
                parser->state = PARSE_MAGIC_LO;
            }
            break;

        case PARSE_HEADER:
            parser->header_buf[parser->header_idx++] = byte;
            parser->crc_running = crc16_ccitt_continue(parser->crc_running, &byte, 1);

            if (parser->header_idx >= 4)
            {
                parser->frame.type = parser->header_buf[0];
                parser->frame.seq = parser->header_buf[1];
                parser->frame.flags = parser->header_buf[2];
                parser->frame.payload_len = parser->header_buf[3];

                if (parser->frame.payload_len > PROTOCOL_MAX_PAYLOAD)
                {
                    protocol_parser_reset(parser);
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
            parser->crc_running = crc16_ccitt_continue(parser->crc_running, &byte, 1);

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
                parser->state = PARSE_MAGIC_LO;
                return 1; /* 帧完成 */
            }
            protocol_parser_reset(parser);
            break;
        }

        default:
            protocol_parser_reset(parser);
            break;
    }

    return 0;
}

int protocol_parse_buf(protocol_parser_t* parser, const uint8_t* data,
                       size_t len, protocol_frame_t* out_frames, size_t max_frames)
{
    int count = 0;
    for (size_t i = 0; i < len && (size_t)count < max_frames; i++)
    {
        if (protocol_parse_byte(parser, data[i]))
        {
            if (out_frames != NULL)
            {
                memcpy(&out_frames[count], &parser->frame, sizeof(protocol_frame_t));
            }
            count++;
        }
    }
    return count;
}

/* ==================== ACK 管理器 ==================== */

void protocol_ack_mgr_init(protocol_ack_mgr_t* mgr)
{
    memset(mgr, 0, sizeof(*mgr));
}

uint8_t protocol_ack_mgr_next_seq(protocol_ack_mgr_t* mgr)
{
    return mgr->next_seq++;
}

int protocol_ack_mgr_add(protocol_ack_mgr_t* mgr, uint8_t seq, uint32_t now_ms)
{
    for (int i = 0; i < PROTOCOL_MAX_PENDING_ACK; i++)
    {
        if (!mgr->slots[i].active)
        {
            mgr->slots[i].seq = seq;
            mgr->slots[i].retries = 0;
            mgr->slots[i].send_time_ms = now_ms;
            mgr->slots[i].active = 1;
            return 0;
        }
    }
    return -1; /* 槽位已满 */
}

int protocol_ack_mgr_check(protocol_ack_mgr_t* mgr, uint8_t ack_seq)
{
    for (int i = 0; i < PROTOCOL_MAX_PENDING_ACK; i++)
    {
        if (mgr->slots[i].active && mgr->slots[i].seq == ack_seq)
        {
            mgr->slots[i].active = 0;
            return 1;
        }
    }
    return 0;
}

int protocol_ack_mgr_check_timeout(protocol_ack_mgr_t* mgr, uint32_t now_ms,
                                   uint8_t* expired_seqs, int max_expired)
{
    int count = 0;
    for (int i = 0; i < PROTOCOL_MAX_PENDING_ACK && count < max_expired; i++)
    {
        if (!mgr->slots[i].active)
        {
            continue;
        }
        const uint32_t elapsed = now_ms - mgr->slots[i].send_time_ms;
        if (elapsed >= PROTOCOL_ACK_TIMEOUT_MS)
        {
            mgr->slots[i].retries++;
            if (mgr->slots[i].retries >= PROTOCOL_ACK_MAX_RETRIES)
            {
                if (expired_seqs != NULL)
                {
                    expired_seqs[count] = mgr->slots[i].seq;
                }
                count++;
                mgr->slots[i].active = 0;
            }
            else
            {
                mgr->slots[i].send_time_ms = now_ms; /* 等待重发 */
            }
        }
    }
    return count;
}

/* ==================== 工具 ==================== */

uint16_t htole16(uint16_t value)
{
    return (uint16_t)(((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8));
}

uint32_t htole32(uint32_t x)
{
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8) |
           ((x & 0x0000FF00) << 8) |
           ((x & 0x000000FF) << 24);
}

void print_hex_data(const uint8_t* data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

size_t buffer_to_hex(const uint8_t* src_buffer, size_t src_len,
                     char* dst_buffer, size_t dst_size)
{
    if (src_buffer == NULL || dst_buffer == NULL || dst_size == 0)
    {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < src_len; i++)
    {
        const int bytes_needed = snprintf(dst_buffer + written, dst_size - written,
                                          "%02X ", src_buffer[i]);
        if (bytes_needed < 0 || written + (size_t)bytes_needed >= dst_size)
        {
            break;
        }
        written += (size_t)bytes_needed;
    }

    if (written > 0 && dst_buffer[written - 1] == ' ')
    {
        dst_buffer[written - 1] = '\0';
        written--;
    }
    else
    {
        dst_buffer[written] = '\0';
    }
    return written;
}
