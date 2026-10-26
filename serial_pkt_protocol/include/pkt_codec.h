#pragma once

/**
 * @file pkt_codec.h
 * @brief UART 协议 v2 编解码器（零 malloc）
 *
 * 帧格式：
 *   MAGIC(2) | TYPE(1) | SEQ(1) | FLAGS(1) | LEN(1) | PAYLOAD(≤120) | CRC16(2)
 *   总帧长 = 8 + payload_len，最大 128 字节
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ==================== 常量 ==================== */

#define PKT_MAGIC         0xAA55
#define PKT_MAGIC_LO      0x55
#define PKT_MAGIC_HI      0xAA
#define PKT_MAX_PAYLOAD   120
#define PKT_FRAME_OVERHEAD 8 /* MAGIC(2)+TYPE(1)+SEQ(1)+FLAGS(1)+LEN(1)+CRC(2) */
#define PKT_MAX_FRAME_SIZE (PKT_FRAME_OVERHEAD + PKT_MAX_PAYLOAD) /* 128 */

/* FLAGS 位定义 */
#define PKT_FLAG_ACK_REQ (1U << 7) /* 请求应答 */
#define PKT_FLAG_ACK_RSP (1U << 6) /* 此帧为应答 */

/* ACK 管理器 */
#define PKT_MAX_PENDING_ACK 4
#define PKT_ACK_TIMEOUT_MS  200
#define PKT_ACK_MAX_RETRIES 3

/* ==================== 类型 ==================== */

/** 协议类型 */
typedef enum {
    PKT_TYPE_SENSOR    = 0x01, /* 传感器数据上报 */
    PKT_TYPE_CONTROL   = 0x02, /* 控制指令 */
    PKT_TYPE_LOG       = 0x03, /* 系统日志 */
    PKT_TYPE_ACK       = 0x04, /* 应答帧 */
    PKT_TYPE_HEARTBEAT = 0x05, /* 心跳 */
    PKT_TYPE_JSON_CMD  = 0x06, /* JSON 命令（手机控制） */
    PKT_TYPE_MOTION    = 0x07, /* 运动状态上报（36 字节） */
} pkt_type_t;

/** 完整帧结构（固定大小，可值传递） */
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t flags;
    uint8_t payload_len;
    uint8_t payload[PKT_MAX_PAYLOAD];
} pkt_frame_t;

/* ==================== 编码器 ==================== */

/**
 * @brief 将帧编码到调用者提供的缓冲区（零 malloc）
 *
 * @param frame       待编码的帧
 * @param out_buf     输出缓冲区（至少 PKT_MAX_FRAME_SIZE 字节）
 * @param out_buf_size 输出缓冲区大小
 * @param out_len     [out] 实际写入的字节数
 * @return ESP_OK 成功，ESP_ERR_INVALID_SIZE 缓冲区太小
 */
esp_err_t pkt_encode(const pkt_frame_t* frame, uint8_t* out_buf,
                     size_t out_buf_size, size_t* out_len);

/* ==================== 解析器 ==================== */

/** 解析器状态 */
typedef enum {
    PARSE_MAGIC_LO, /* 等待 0x55 */
    PARSE_MAGIC_HI, /* 等待 0xAA */
    PARSE_HEADER,   /* 收集 type/seq/flags/len (4 字节) */
    PARSE_PAYLOAD,  /* 收集 payload */
    PARSE_CRC_LO,   /* 收集 CRC 低字节 */
    PARSE_CRC_HI,   /* 收集 CRC 高字节，校验 */
} pkt_parse_state_t;

/** 解析器实例（~150 字节，静态分配） */
typedef struct {
    pkt_parse_state_t state;
    pkt_frame_t frame;
    uint8_t header_idx;    /* 当前 header 接收位置 (0-3) */
    uint8_t header_buf[4]; /* type/seq/flags/len */
    uint16_t crc_running;  /* 渐进 CRC */
} pkt_parser_t;

/** 初始化解析器 */
void pkt_parser_init(pkt_parser_t* parser);

/** 重置解析器到初始状态 */
void pkt_parser_reset(pkt_parser_t* parser);

/**
 * @brief 喂入一个字节
 * @return true 表示一帧解析完成（结果在 parser->frame 中）
 */
bool pkt_parser_feed(pkt_parser_t* parser, uint8_t byte);

/**
 * @brief 批量喂入数据
 * @param parser     解析器
 * @param data       输入数据
 * @param len        数据长度
 * @param out_frames [out] 完成的帧数组
 * @param max_frames 数组容量
 * @return 完成的帧数
 */
int pkt_parser_feed_buf(pkt_parser_t* parser, const uint8_t* data,
                        size_t len, pkt_frame_t* out_frames,
                        size_t max_frames);

/** 每解析完成一帧时回调（frame 指向 parser 内部缓冲区，仅在回调期间有效） */
typedef void (*pkt_frame_cb_t)(const pkt_frame_t* frame, void* ctx);

/**
 * @brief 流式喂入数据：把整个 chunk 解析完，每完成一帧回调一次
 *
 * uart_service 用它处理一次 UART 读取（可能包含多帧）。
 *
 * @return 完成的帧数
 */
int pkt_parser_feed_stream(pkt_parser_t* parser, const uint8_t* data,
                           size_t len, pkt_frame_cb_t cb, void* ctx);

/* ==================== ACK 管理器 ==================== */

/** 单个 pending ACK 槽位 */
typedef struct {
    uint8_t seq;
    uint8_t retries;
    uint32_t send_time_ms;
    bool active;
} pkt_pending_ack_t;

/** ACK 管理器（静态分配） */
typedef struct {
    pkt_pending_ack_t slots[PKT_MAX_PENDING_ACK];
    uint8_t next_seq;
} pkt_ack_mgr_t;

/** 初始化 ACK 管理器 */
void pkt_ack_mgr_init(pkt_ack_mgr_t* mgr);

/** 获取下一个序列号（0-255 循环） */
uint8_t pkt_ack_mgr_next_seq(pkt_ack_mgr_t* mgr);

/** 添加一个 pending ACK */
esp_err_t pkt_ack_mgr_add(pkt_ack_mgr_t* mgr, uint8_t seq, uint32_t now_ms);

/** 检查是否收到 ACK（匹配则移除） */
bool pkt_ack_mgr_check(pkt_ack_mgr_t* mgr, uint8_t ack_seq);

/**
 * @brief 检查超时的 ACK
 * @param expired_seqs [out] 超时的 seq 数组
 * @param max_expired  数组容量
 * @return 超时的 ACK 数量
 */
int pkt_ack_mgr_check_timeout(pkt_ack_mgr_t* mgr, uint32_t now_ms,
                              uint8_t* expired_seqs, int max_expired);
