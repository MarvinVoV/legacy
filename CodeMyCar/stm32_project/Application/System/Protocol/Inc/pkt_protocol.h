/*
 * pkt_protocol.h
 *
 * UART 协议 v2 编解码器（与 esp32/components/pkt_codec 单一事实源一致）
 *
 * 帧格式（小端）：
 *   MAGIC(2): 0x55 0xAA | TYPE(1) | SEQ(1) | FLAGS(1) | LEN(1) | PAYLOAD(≤120) | CRC16(2)
 *   总帧长 = 8 + payload_len，最大 128 字节
 *   CRC16-CCITT（多项式 0x1021，初始 0xFFFF），覆盖 MAGIC..PAYLOAD
 */

#ifndef SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_H_
#define SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_H_

#include <stdint.h>
#include <stdlib.h>

/* ==================== 帧常量 ==================== */

#define FRAME_MAGIC_LO      0x55   /* wire 首字节 */
#define FRAME_MAGIC_HI      0xAA   /* wire 次字节 */
#define PROTOCOL_MAX_PAYLOAD 120
#define PROTOCOL_FRAME_OVERHEAD 8  /* MAGIC(2)+TYPE(1)+SEQ(1)+FLAGS(1)+LEN(1)+CRC(2) */
#define PROTOCOL_MAX_FRAME_SIZE (PROTOCOL_FRAME_OVERHEAD + PROTOCOL_MAX_PAYLOAD) /* 128 */

/* 兼容旧名 */
#define PROTOCOL_MAX_DATA_LEN PROTOCOL_MAX_PAYLOAD

/* FLAGS 位定义 */
#define FRAME_FLAG_ACK_REQ (1U << 7) /* 请求应答 */
#define FRAME_FLAG_ACK_RSP (1U << 6) /* 此帧为应答 */

/* ACK 管理器参数（与 ESP32 一致） */
#define PROTOCOL_MAX_PENDING_ACK 4
#define PROTOCOL_ACK_TIMEOUT_MS  200
#define PROTOCOL_ACK_MAX_RETRIES 3

/* ==================== 类型 ==================== */

/** 协议类型（值与 esp32 pkt_type_t 对齐） */
typedef enum
{
    PROTOCOL_TYPE_MIN       = 0x00,
    PROTOCOL_TYPE_SENSOR    = 0x01, /* 传感器数据上报 */
    PROTOCOL_TYPE_CONTROL   = 0x02, /* 控制指令 */
    PROTOCOL_TYPE_LOG       = 0x03, /* 系统日志 */
    PROTOCOL_TYPE_ACK       = 0x04, /* 应答帧 */
    PROTOCOL_TYPE_HEARTBEAT = 0x05, /* 心跳 */
    PROTOCOL_TYPE_JSON_CMD  = 0x06, /* JSON 命令（手机控制，预留） */
    PROTOCOL_TYPE_MOTION    = 0x07, /* 运动状态上报（36 字节） */
    PROTOCOL_TYPE_MAX
} protocol_type_t;

/** 完整帧结构（固定大小，可值传递，零 malloc 解析） */
typedef struct
{
    uint8_t type;
    uint8_t seq;
    uint8_t flags;
    uint8_t payload_len;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
} protocol_frame_t;

/* ==================== 编码器 ==================== */

/**
 * @brief 将帧编码到调用者缓冲区（零 malloc 版本）
 * @param frame       待编码的帧
 * @param out_buf     输出缓冲区（至少 PROTOCOL_MAX_FRAME_SIZE 字节）
 * @param out_buf_size 缓冲区大小
 * @param out_len     [out] 实际写入字节数
 * @return 0 成功，非 0 失败（-1 参数，-2 超长/缓冲不足）
 */
int protocol_encode(const protocol_frame_t* frame, uint8_t* out_buf,
                    size_t out_buf_size, size_t* out_len);

/**
 * @brief 打包协议帧（malloc 版本，兼容旧 TX 路径）
 * @note  seq=0, flags=0；调用者负责 free()
 * @return malloc 出的帧缓冲区，失败返回 NULL
 */
uint8_t* protocol_pack_frame(protocol_type_t type, const uint8_t* data,
                             uint16_t data_len, uint16_t* frame_len);

/**
 * @brief 打包协议帧（带 seq/flags，用于 ACK 回复）
 * @note  调用者负责 free()
 */
uint8_t* protocol_pack_frame_ex(protocol_type_t type, uint8_t seq, uint8_t flags,
                                const uint8_t* data, uint16_t data_len,
                                uint16_t* frame_len);

/* ==================== 解析器 ==================== */

/** 解析器状态 */
typedef enum
{
    PARSE_MAGIC_LO, /* 等待 0x55 */
    PARSE_MAGIC_HI, /* 等待 0xAA */
    PARSE_HEADER,   /* 收集 type/seq/flags/len (4 字节) */
    PARSE_PAYLOAD,  /* 收集 payload */
    PARSE_CRC_LO,   /* CRC 低字节 */
    PARSE_CRC_HI    /* CRC 高字节，校验 */
} parse_state_t;

/** 解析器实例（静态 payload，零 malloc） */
typedef struct
{
    parse_state_t state;
    protocol_frame_t frame;
    uint8_t header_idx;    /* header/payload 接收位置 */
    uint8_t header_buf[4]; /* type/seq/flags/len 暂存 + CRC 低字节暂存 */
    uint16_t crc_running;  /* 渐进 CRC */
} protocol_parser_t;

void protocol_parser_init(protocol_parser_t* parser);
void protocol_parser_reset(protocol_parser_t* parser);

/**
 * @brief 单字节解析
 * @return 1 表示一帧解析完成（结果在 parser->frame 中），0 未完成
 */
int protocol_parse_byte(protocol_parser_t* parser, uint8_t byte);

/**
 * @brief 批量喂入数据
 * @return 完成的帧数
 */
int protocol_parse_buf(protocol_parser_t* parser, const uint8_t* data,
                       size_t len, protocol_frame_t* out_frames, size_t max_frames);

/* ==================== CRC16-CCITT ==================== */

uint16_t crc16_ccitt(const uint8_t* data, uint16_t length);
uint16_t crc16_ccitt_continue(uint16_t crc, const uint8_t* data, uint16_t length);

/* ==================== ACK 管理器 ==================== */

typedef struct
{
    uint8_t seq;
    uint8_t retries;
    uint32_t send_time_ms;
    uint8_t active;
} protocol_pending_ack_t;

typedef struct
{
    protocol_pending_ack_t slots[PROTOCOL_MAX_PENDING_ACK];
    uint8_t next_seq;
} protocol_ack_mgr_t;

void protocol_ack_mgr_init(protocol_ack_mgr_t* mgr);
uint8_t protocol_ack_mgr_next_seq(protocol_ack_mgr_t* mgr);
int protocol_ack_mgr_add(protocol_ack_mgr_t* mgr, uint8_t seq, uint32_t now_ms);
int protocol_ack_mgr_check(protocol_ack_mgr_t* mgr, uint8_t ack_seq);
int protocol_ack_mgr_check_timeout(protocol_ack_mgr_t* mgr, uint32_t now_ms,
                                   uint8_t* expired_seqs, int max_expired);

/* ==================== 工具 ==================== */

uint16_t htole16(uint16_t value);
uint32_t htole32(uint32_t x);
void print_hex_data(const uint8_t* data, uint16_t len);
size_t buffer_to_hex(const uint8_t* src_buffer, size_t src_len,
                     char* dst_buffer, size_t dst_size);

#endif /* SYSTEM_PROTOCOL_INC_PKT_PROTOCOL_H_ */
