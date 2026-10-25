#pragma once

/**
 * @file cmcar_crc16.h
 * @brief CRC16-CCITT 校验（多项式 0x1021，初始值 0xFFFF）
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 计算 CRC16-CCITT
 * @param data 数据缓冲区
 * @param len  数据长度
 * @return CRC16 校验值
 */
uint16_t cmcar_crc16(const uint8_t* data, size_t len);

/**
 * @brief 增量 CRC 计算（在已有 CRC 基础上继续）
 * @param crc  当前 CRC 值
 * @param data 数据缓冲区
 * @param len  数据长度
 * @return 更新后的 CRC16 值
 */
uint16_t cmcar_crc16_continue(uint16_t crc, const uint8_t* data, size_t len);
