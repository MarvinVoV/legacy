/**
 * @file cmcar_crc16.c
 * @brief CRC16-CCITT 实现
 */

#include "cmcar_crc16.h"

#define CRC16_INIT 0xFFFF
#define CRC16_POLY 0x1021

uint16_t cmcar_crc16_continue(uint16_t crc, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (uint16_t)((crc << 1) ^ CRC16_POLY);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t cmcar_crc16(const uint8_t* data, size_t len)
{
    return cmcar_crc16_continue(CRC16_INIT, data, len);
}
