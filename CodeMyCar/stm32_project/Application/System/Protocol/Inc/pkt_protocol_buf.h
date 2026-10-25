/*
 * pkt_protocol_buf.h
 *
 * 协议接收器：封装 v2 状态机解析器，对 DMA 分片输入透明。
 * 零 per-frame malloc；解析完成的帧通过回调投递。
 */

#ifndef SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_BUF_H_
#define SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_BUF_H_

#include "pkt_protocol.h"
#include <stdint.h>

/**
 * @brief 帧接收回调（类型 + seq + flags + payload + 长度）
 * @note  data 指向 parser 内部静态 payload，回调返回后即失效，需自行拷贝。
 */
typedef void (*frame_callback)(uint8_t type, uint8_t seq, uint8_t flags,
                               const uint8_t* data, uint16_t len);

/**
 * @brief 协议接收器（零 malloc；状态保存在 parser 中）
 */
typedef struct
{
    protocol_parser_t parser; /* 状态机解析器 */
    frame_callback    callback;
} protocol_receiver;

/**
 * @brief 初始化接收器
 * @param receiver 接收器
 * @param buf_size 兼容旧参数（v2 状态机无需累积缓冲，忽略）
 * @param callback 帧回调
 */
void protocol_receiver_init(protocol_receiver* receiver, uint16_t buf_size,
                            frame_callback callback);

/**
 * @brief 追加数据并解析（可跨 DMA 分片）
 */
void protocol_receiver_append(protocol_receiver* receiver, const uint8_t* data, uint16_t len);

/**
 * @brief 销毁接收器（v2 无动态资源，保留以兼容旧 API）
 */
void protocol_receiver_destroy(protocol_receiver* receiver);

#endif /* SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_BUF_H_ */
