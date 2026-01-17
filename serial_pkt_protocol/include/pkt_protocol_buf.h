//
// Created by marvin on 2025/2/14.
//

#ifndef PKT_PROTOCOL_BUF_H
#define PKT_PROTOCOL_BUF_H

#include "pkt_protocol.h"
#include <stdint.h>
#include <stdlib.h>

// 声明外部可访问的 mock_realloc
extern void* (*mock_realloc)(void* ptr, size_t size);


/**
 * @brief 用户自定义回调函数类型（协议类型 + 数据 + 长度）
 */
typedef void (*frame_callback)(uint8_t type, const uint8_t* data, uint16_t len);


/**
 * @brief 协议接收器结构体（封装缓冲区、解析状态）
 */
typedef struct
{
    uint8_t* buffer; // 动态分配的缓冲区
    uint16_t buffer_size; // 缓冲区总大小
    uint16_t write_pos; // 当前写入位置
    uint16_t processed_pos; // 跟踪解析处理位置
    protocol_parser_t parser; // 协议解析器
    frame_callback callback; // 用户回调函数
} protocol_receiver;


/**
 * @brief 初始化协议接收器
 * @param receiver  接收器对象
 * @param buf_size  缓冲区大小（需大于最大帧长度）
 * @param callback  数据帧接收完成回调函数
 */
void protocol_receiver_init(protocol_receiver* receiver, uint16_t buf_size, frame_callback callback);

/**
 * @brief 向接收器追加新接收到的数据
 * @param receiver  接收器对象
 * @param data      新数据指针
 * @param len       新数据长度
 */
void protocol_receiver_append(protocol_receiver* receiver, const uint8_t* data, uint16_t len);

/**
 * @brief 销毁接收器，释放资源
 */
void protocol_receiver_destroy(protocol_receiver* receiver);

#endif //PKT_PROTOCOL_BUF_H
