/*
 * pkt_protocol_buf.h
 *
 * 协议接收器：封装 v2 状态机解析器，对分片输入透明。零 per-frame malloc。
 */
#ifndef SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_BUF_H
#define SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_BUF_H

#include "pkt_protocol.h"
#include <stdint.h>

/**
 * @brief 帧接收回调（类型 + seq + flags + payload + 长度）
 * @note  data 指向 parser 内部静态 payload，回调返回后即失效，需自行拷贝。
 */
typedef void (*frame_callback)(uint8_t type, uint8_t seq, uint8_t flags,
                               const uint8_t* data, uint16_t len);

typedef struct
{
    protocol_parser_t parser;
    frame_callback    callback;
} protocol_receiver;

void protocol_receiver_init(protocol_receiver* receiver, uint16_t buf_size,
                            frame_callback callback);
void protocol_receiver_append(protocol_receiver* receiver, const uint8_t* data, uint16_t len);
void protocol_receiver_destroy(protocol_receiver* receiver);

#endif /* SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_BUF_H */
