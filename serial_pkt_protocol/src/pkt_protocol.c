#include "pkt_protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


static uint16_t crc16_ccitt_continue(uint16_t crc, const uint8_t* data,
                                     uint16_t length);
// 封装协议数据帧
uint8_t* protocol_pack_frame(const protocol_type_t type, const uint8_t* data,
                             uint16_t data_len, uint16_t* frame_len)
{
    if (data_len > PROTOCOL_MAX_DATA_LEN)
    {
        printf("pack: data length too long");
        return NULL;
    }

    // 计算总长度: Header(5) + Data(data_len) + CRC(2) + End(2)
    *frame_len = sizeof(protocol_header_t) + data_len + sizeof(uint16_t) +
        sizeof(uint16_t);
    uint8_t* frame = malloc(*frame_len);
    if (!frame)
    {
        printf("pack: malloc failed");
        return NULL;
    }

    protocol_header_t* header = (protocol_header_t*)frame;
    header->header = TO_LE16(FRAME_HEADER);
    header->type = type;
    header->len = TO_LE16(data_len);
    memcpy(header->data, data, data_len);

    const uint16_t crc = crc16_ccitt(frame, sizeof(protocol_header_t) + data_len);
    memcpy(frame + sizeof(protocol_header_t) + data_len, &crc, sizeof(crc));

    const uint16_t tail = TO_LE16(FRAME_TAIL);
    memcpy(frame + sizeof(protocol_header_t) + data_len + sizeof(crc), &tail,
           sizeof(tail));

    return frame;
}

// 解析器初始化
void protocol_parser_init(protocol_parser_t* parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = STATE_WAIT_HEADER_1;
}

void protocol_parser_reset(protocol_parser_t* parser)
{
    protocol_parser_init(parser);
    // 如果有动态分配的数据缓冲区需要释放：
    if (parser->frame.data != NULL)
    {
        free(parser->frame.data);
        parser->frame.data = NULL;
    }
}

// 解析协议数据流
int protocol_parse_byte(protocol_parser_t* parser, uint8_t byte)
{
    switch (parser->state)
    {
    case STATE_WAIT_HEADER_1:
        if (byte == (FRAME_HEADER & 0xFF))
        {
            parser->state = STATE_WAIT_HEADER_2;
        }
        break;
    case STATE_WAIT_HEADER_2:
        // 注意 小端
        if (byte == (FRAME_HEADER >> 8))
        {
            parser->frame.header = TO_LE16(FRAME_HEADER);
            parser->state = STATE_WAIT_TYPE;
        }
        else
        {
            parser->state = STATE_WAIT_HEADER_1;
        }
        break;
    case STATE_WAIT_TYPE:
        parser->frame.type = byte;
        parser->state = STATE_WAIT_LENGTH_1;
        break;
    case STATE_WAIT_LENGTH_1:
        parser->frame.len = byte;
        parser->state = STATE_WAIT_LENGTH_2;
        break;
    case STATE_WAIT_LENGTH_2:
        parser->frame.len |= (byte << 8);
        parser->frame.len = TO_LE16(parser->frame.len);
        parser->frame.data = (uint8_t*)malloc(parser->frame.len);
        parser->data_index = 0;
        parser->state = STATE_WAIT_DATA;
        break;
    case STATE_WAIT_DATA:
        parser->frame.data[parser->data_index++] = byte;
        if (parser->data_index >= parser->frame.len)
        {
            parser->state = STATE_WAIT_CRC_1;
        }
        break;
    case STATE_WAIT_CRC_1:
        parser->frame.crc = byte;
        parser->state = STATE_WAIT_CRC_2;
        break;
    case STATE_WAIT_CRC_2:
        parser->frame.crc |= (byte << 8);
        parser->frame.crc = TO_LE16(parser->frame.crc);
        parser->state = STATE_WAIT_TAIL_1;
        break;
    case STATE_WAIT_TAIL_1:
        if (byte == (FRAME_TAIL & 0xFF))
        {
            parser->state = STATE_WAIT_TAIL_2;
        }
        else
        {
            free(parser->frame.data);
            parser->state = STATE_WAIT_HEADER_1;
        }
        break;
    case STATE_WAIT_TAIL_2:
        if (byte == (FRAME_TAIL >> 8))
        {
            // 构造协议头和数据部分的字节流
            uint8_t header_part[5];
            header_part[0] =
                (uint8_t)(parser->frame.header & 0xFF); // Header低字节
            header_part[1] =
                (uint8_t)((parser->frame.header >> 8) & 0xFF); // Header高字节
            header_part[2] = parser->frame.type; // 类型
            header_part[3] = (uint8_t)(parser->frame.len & 0xFF); // 长度低字节
            header_part[4] =
                (uint8_t)((parser->frame.len >> 8) & 0xFF); // 长度高字节

            // 计算CRC：协议头 + 数据
            uint16_t crc = crc16_ccitt(header_part, sizeof(header_part));
            if (parser->frame.len > 0)
            {
                crc =
                    crc16_ccitt_continue(crc, parser->frame.data, parser->frame.len);
            }

            if (parser->frame.crc == crc)
            {
                return 1; // 解析成功
            }
        }
        free(parser->frame.data);
        parser->state = STATE_WAIT_HEADER_1;
        break;
    }
    return 0;
}

/**
 * 计算CRC16校验码
 */
uint16_t crc16_ccitt(const uint8_t* data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint16_t crc16_ccitt_continue(uint16_t crc, const uint8_t* data,
                                     uint16_t length)
{
    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

uint16_t htole16(const uint16_t value)
{
    return ((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8);
}

uint32_t htole32(const uint32_t x)
{
    return ((x & 0xFF000000) >> 24) |
        ((x & 0x00FF0000) >> 8) |
        ((x & 0x0000FF00) << 8) |
        ((x & 0x000000FF) << 24);
}

void print_hex_data(const uint8_t* data, const uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}