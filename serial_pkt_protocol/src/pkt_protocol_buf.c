//
// Created by marvin on 2025/2/14.
//
#include "pkt_protocol.h"
#include "pkt_protocol_buf.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>


/**
 * 尝试从缓冲区解析完整帧
 * @param receiver   协议接收器结构体指针
 */
static void try_parse_frame(protocol_receiver* receiver)
{
    // 记录未处理数据的起始位置
    size_t unprocessed_start = 0;
    while (receiver->processed_pos < receiver->write_pos)
    {
        const uint8_t byte = receiver->buffer[receiver->processed_pos];
        if (protocol_parse_byte(&receiver->parser, byte))
        {
            // 解析成功，计算预期帧长
            const uint16_t expect_frame_len = sizeof(protocol_header_t) + receiver->parser.frame.len + sizeof(uint16_t)
                * 2;

            // 计算帧起始位置并校验合法性
            const size_t frame_start_pos = receiver->processed_pos + 1 - expect_frame_len;
            const size_t frame_end_pos = receiver->processed_pos + 1;
            // 校验帧起始位置合法性（严格限定在缓冲区范围内）
            const bool valid_frame_boundary = (frame_start_pos <= receiver->write_pos) &&
                (frame_end_pos <= receiver->write_pos);
            if (valid_frame_boundary)
            {
                if (receiver->callback)
                {
                    receiver->callback(receiver->parser.frame.type, receiver->parser.frame.data,
                                       receiver->parser.frame.len);
                }
                // 直接跳到帧末尾，跳过已处理数据
                receiver->processed_pos = frame_end_pos;
                // 更新未处理数据起始点
                unprocessed_start = receiver->processed_pos;
            }
            else
            {
                receiver->processed_pos++;
            }
            // 无论是否成功，重置解析器
            protocol_parser_reset(&receiver->parser);
        }
        else
        {
            // 解析未完成，正常推进
            receiver->processed_pos++;
        }
    }
    // 移动未处理数据到缓冲区头部
    if (unprocessed_start > 0)
    {
        size_t remaining = receiver->write_pos - unprocessed_start;
        memmove(receiver->buffer, receiver->buffer + unprocessed_start, remaining);
        receiver->write_pos = remaining;
        receiver->processed_pos = 0;
    }
}


/**
 * @brief 初始化协议接收器
 * @param receiver   协议接收器结构体指针
 * @param buf_size   缓冲区大小
 * @param callback   帧回调函数
 */
void protocol_receiver_init(protocol_receiver* receiver, const uint16_t buf_size, const frame_callback callback)
{
    receiver->buffer = (uint8_t*)malloc(buf_size);
    receiver->buffer_size = buf_size;
    receiver->write_pos = 0;
    receiver->processed_pos = 0;
    receiver->callback = callback;
}


/**
 * @brief 追加新数据并尝试解析
 * @param receiver   协议接收器结构体指针
 * @param data       新数据指针
 * @param len        新数据长度
 */
void protocol_receiver_append(protocol_receiver* receiver, const uint8_t* data, uint16_t len)
{
    // ------------------ 缓冲区溢出情况处理 ---------------------
    // 情况1：缓冲区剩余空间不足，但可以通过移动未处理数据腾出空间
    if (receiver->write_pos + len > receiver->buffer_size)
    {
        // 计算已处理数据长度
        const size_t processed_len = receiver->processed_pos;
        if (processed_len > 0)
        {
            // 移动未处理数据到缓冲区头部
            size_t remaining = receiver->write_pos - processed_len;
            memmove(receiver->buffer, receiver->buffer + processed_len, remaining);
            receiver->write_pos = remaining;
            receiver->processed_pos = 0;
        }
        // 情况2：移动后剩余空间仍不足，需动态扩容或丢弃数据
        if (receiver->write_pos + len > receiver->buffer_size)
        {
            // 动态扩容策略（扩容为原大小的2倍）
            const uint16_t new_size = receiver->buffer_size * 2;
            const uint8_t* new_buf = realloc(receiver->buffer, new_size);
            if (!new_buf)
            {
                // 动态扩容失败：尝试部分写入
                const uint16_t available_space = receiver->buffer_size - receiver->write_pos;
                if (available_space > 0)
                {
                    // 部分写入：将新数据追加到缓冲区末尾
                    memcpy(receiver->buffer + receiver->write_pos, data, available_space);
                    receiver->write_pos += available_space;
                    printf("Error: Partial data written (%d bytes), discarded %d bytes\n",
                           available_space, len - available_space);
                }
                else
                {
                    printf("Error: All new data discarded: %d bytes\n", len);
                }
                try_parse_frame(receiver);
                return;
            }
            receiver->buffer = (uint8_t*)new_buf;
            receiver->buffer_size = new_size;
        }
    }

    // -------------------------- 二次空间检查（防御性编程）--------------------------
    const uint16_t available_space = receiver->buffer_size - receiver->write_pos;
    if (available_space < len)
    {
        // 处理空间不一致的极端情况（如并发写入）
        printf("Error: Buffer space check failed: require=%d, available=%d\n", len, available_space);
        len = available_space; // 强制裁剪
        if (len == 0)
        {
            return;
        }
    }
    // 追加数据到缓冲区
    memcpy(receiver->buffer + receiver->write_pos, data, len);
    receiver->write_pos += len;

    // 尝试解析完整帧
    try_parse_frame(receiver);
}


/**
 * @brief 释放协议接收器资源
 * @param receiver 协议接收器结构体指针
 */
void protocol_receiver_destroy(protocol_receiver* receiver)
{
    free(receiver->buffer);
    receiver->buffer = NULL;
    receiver->buffer_size = 0;
    receiver->processed_pos = 0;
    receiver->write_pos = 0;
}
