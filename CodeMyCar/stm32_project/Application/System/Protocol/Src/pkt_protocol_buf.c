/*
 * pkt_protocol_buf.c
 *
 * 协议接收器实现：状态机解析器包装，零 per-frame malloc。
 */

#include "pkt_protocol.h"
#include "pkt_protocol_buf.h"

void protocol_receiver_init(protocol_receiver* receiver, uint16_t buf_size,
                            frame_callback callback)
{
    (void)buf_size; /* v2 状态机无需累积缓冲 */
    protocol_parser_init(&receiver->parser);
    receiver->callback = callback;
}

void protocol_receiver_append(protocol_receiver* receiver, const uint8_t* data, uint16_t len)
{
    if (receiver == NULL || data == NULL)
    {
        return;
    }
    for (uint16_t i = 0; i < len; i++)
    {
        if (protocol_parse_byte(&receiver->parser, data[i]))
        {
            const protocol_frame_t* f = &receiver->parser.frame;
            if (receiver->callback != NULL)
            {
                receiver->callback(f->type, f->seq, f->flags,
                                   f->payload, f->payload_len);
            }
            /* 解析完成，parser 已自动回到 PARSE_MAGIC_LO */
        }
    }
}

void protocol_receiver_destroy(protocol_receiver* receiver)
{
    if (receiver == NULL)
    {
        return;
    }
    protocol_parser_init(&receiver->parser);
    receiver->callback = NULL;
}
