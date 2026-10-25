#pragma once

/*
 * cmcar_crc16.h（host 测试 shim）
 *
 * ESP32 的 pkt_codec.c 调用 cmcar_crc16()/cmcar_crc16_continue()；
 * host 侧对应实现是 pkt_protocol.c 里的 crc16_ccitt()/crc16_ccitt_continue()，
 * 算法完全一致（CRC16-CCITT, poly 0x1021, init 0xFFFF）。
 */

#include <stddef.h>
#include <stdint.h>

uint16_t crc16_ccitt(const uint8_t* data, uint16_t length);
uint16_t crc16_ccitt_continue(uint16_t crc, const uint8_t* data, uint16_t length);

static inline uint16_t cmcar_crc16(const uint8_t* data, size_t len)
{
    return crc16_ccitt(data, (uint16_t)len);
}

static inline uint16_t cmcar_crc16_continue(uint16_t crc, const uint8_t* data, size_t len)
{
    return crc16_ccitt_continue(crc, data, (uint16_t)len);
}
