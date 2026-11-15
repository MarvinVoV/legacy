/*
 * pkt_protocol.h
 *
 * UART 协议 v2 编解码器（与 esp32/components/pkt_codec、stm32 Application/System/Protocol
 * 单一事实源一致）。
 *
 * 帧格式（小端）：
 *   MAGIC(2): 0x55 0xAA | TYPE(1) | SEQ(1) | FLAGS(1) | LEN(1) | PAYLOAD(≤120) | CRC16(2)
 *   总帧长 = 8 + payload_len，最大 128 字节
 *   CRC16-CCITT（多项式 0x1021，初始 0xFFFF），覆盖 MAGIC..PAYLOAD
 */
#ifndef SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_H
#define SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_H

#include <stdint.h>
#include <stdlib.h>

/* ==================== 帧常量 ==================== */

#define FRAME_MAGIC_LO      0x55
#define FRAME_MAGIC_HI      0xAA
#define PROTOCOL_MAX_PAYLOAD 120
#define PROTOCOL_FRAME_OVERHEAD 8
#define PROTOCOL_MAX_FRAME_SIZE (PROTOCOL_FRAME_OVERHEAD + PROTOCOL_MAX_PAYLOAD)

#define PROTOCOL_MAX_DATA_LEN PROTOCOL_MAX_PAYLOAD

#define FRAME_FLAG_ACK_REQ (1U << 7)
#define FRAME_FLAG_ACK_RSP (1U << 6)

#define PROTOCOL_MAX_PENDING_ACK 4
#define PROTOCOL_ACK_TIMEOUT_MS  200
#define PROTOCOL_ACK_MAX_RETRIES 3

/* ==================== 类型 ==================== */

typedef enum
{
    PROTOCOL_TYPE_MIN       = 0x00,
    PROTOCOL_TYPE_SENSOR    = 0x01,
    PROTOCOL_TYPE_CONTROL   = 0x02,
    PROTOCOL_TYPE_LOG       = 0x03,
    PROTOCOL_TYPE_ACK       = 0x04,
    PROTOCOL_TYPE_HEARTBEAT = 0x05,
    PROTOCOL_TYPE_JSON_CMD  = 0x06,
    PROTOCOL_TYPE_MAX
} protocol_type_t;

typedef struct
{
    uint8_t type;
    uint8_t seq;
    uint8_t flags;
    uint8_t payload_len;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
} protocol_frame_t;

/* ==================== 编码器 ==================== */

int protocol_encode(const protocol_frame_t* frame, uint8_t* out_buf,
                    size_t out_buf_size, size_t* out_len);

uint8_t* protocol_pack_frame(protocol_type_t type, const uint8_t* data,
                             uint16_t data_len, uint16_t* frame_len);

uint8_t* protocol_pack_frame_ex(protocol_type_t type, uint8_t seq, uint8_t flags,
                                const uint8_t* data, uint16_t data_len,
                                uint16_t* frame_len);

/* ==================== 解析器 ==================== */

typedef enum
{
    PARSE_MAGIC_LO,
    PARSE_MAGIC_HI,
    PARSE_HEADER,
    PARSE_PAYLOAD,
    PARSE_CRC_LO,
    PARSE_CRC_HI
} parse_state_t;

typedef struct
{
    parse_state_t state;
    protocol_frame_t frame;
    uint8_t header_idx;
    uint8_t header_buf[4];
    uint16_t crc_running;
} protocol_parser_t;

void protocol_parser_init(protocol_parser_t* parser);
void protocol_parser_reset(protocol_parser_t* parser);
int protocol_parse_byte(protocol_parser_t* parser, uint8_t byte);
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

#endif /* SERIAL_PKT_PROTOCOL_PKT_PROTOCOL_H */
