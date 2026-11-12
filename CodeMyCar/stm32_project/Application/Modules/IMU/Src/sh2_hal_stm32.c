#include "imu_i2c_transport.h"
/*
 * sh2_hal_stm32.c
 *
 * Hillcrest SH-2 协议栈的 STM32 传输层（I2C1 + RST）
 *
 * 引脚：SCL = PB6，SDA = PB7，RST = PB5（低有效）
 * 地址：0x4A（AD0 接地）
 *
 * 参考 Adafruit_BNO08x 的 i2chal_* 实现：
 *   read() 返回一整包（含 4 字节 SHTP 头），长度按包头字段解析
 */

#include "sh2_hal.h"

#include <stdbool.h>
#include <string.h>

#include "stm32h7xx_hal.h"

extern I2C_HandleTypeDef hi2c1;

#ifndef CMCAR_IMU_ASYNC_I2C
#define CMCAR_IMU_ASYNC_I2C 1
#endif
static HAL_StatusTypeDef transfer(bool receive, uint16_t address,
                                  uint8_t *buffer, uint16_t size, uint32_t timeout)
{
#if CMCAR_IMU_ASYNC_I2C
    return ImuI2c_Transfer(receive, address, buffer, size, timeout);
#else
    return receive ? HAL_I2C_Master_Receive(&hi2c1,address,buffer,size,timeout)
                   : HAL_I2C_Master_Transmit(&hi2c1,address,buffer,size,timeout);
#endif
}

static uint16_t s_i2c_addr8 = (0x4Au << 1); /* 打开时自动探测 0x4A/0x4B */
#define SH2_RST_PORT GPIOB
#define SH2_RST_PIN GPIO_PIN_5
#define SH2_I2C_TIMEOUT_MS 20u
#define SH2_MAX_PACKET 384u

/* 诊断：读到的包计数与最近包头 */
volatile uint32_t sh2_diag_reads = 0u;
volatile uint32_t sh2_diag_packets = 0u;
volatile uint8_t sh2_diag_last_hdr[4] = {0};
volatile uint8_t sh2_diag_last_pl[4] = {0};

static uint32_t hal_getTimeUs(sh2_Hal_t* self)
{
    (void)self;
    return (uint32_t)(HAL_GetTick() * 1000u);
}

static void rst_pin_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = SH2_RST_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SH2_RST_PORT, &g);
    HAL_GPIO_WritePin(SH2_RST_PORT, SH2_RST_PIN, GPIO_PIN_SET);
}

static int hal_open(sh2_Hal_t* self)
{
    (void)self;

    rst_pin_init();

    /* 自动探测地址：AD0 悬空时可能是 0x4A 或 0x4B */
    s_i2c_addr8 = 0u;
    const uint16_t candidates[2] = {(uint16_t)(0x4Au << 1), (uint16_t)(0x4Bu << 1)};
    for (uint8_t i = 0u; i < 2u; i++)
    {
        if (HAL_I2C_IsDeviceReady(&hi2c1, candidates[i], 3u, 50u) == HAL_OK)
        {
            s_i2c_addr8 = candidates[i];
            break;
        }
    }
    if (s_i2c_addr8 == 0u)
    {
        s_i2c_addr8 = (uint16_t)(0x4Au << 1); /* 都探测不到时保留默认，后续会超时重试 */
    }

    /* 硬复位：RST 低 10 ms */
    HAL_GPIO_WritePin(SH2_RST_PORT, SH2_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(SH2_RST_PORT, SH2_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(120); /* t1 ≈ 90 ms + 余量 */

    /* 软复位命令（与 Adafruit 相同：{len=5, ch=1, seq=0, RESET=1}） */
    uint8_t softreset_pkt[5] = {5u, 0u, 1u, 0u, 1u};
    for (uint8_t attempt = 0u; attempt < 5u; attempt++)
    {
        if (transfer(false, s_i2c_addr8, softreset_pkt, 5u,
                                    SH2_I2C_TIMEOUT_MS) == HAL_OK)
        {
            HAL_Delay(300);
            return 0;
        }
        HAL_Delay(30);
    }
    return -1;
}

static void hal_close(sh2_Hal_t* self)
{
    (void)self;
}

static int hal_read(sh2_Hal_t* self, uint8_t* pBuffer, unsigned len, uint32_t* t_us)
{
    (void)self;

    uint8_t header[4];
    sh2_diag_reads++;

    if (transfer(true, s_i2c_addr8, header, 4u,
                               SH2_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return 0;
    }

    uint16_t packet_size = (uint16_t)((uint16_t)header[0] | ((uint16_t)header[1] << 8));
    packet_size = (uint16_t)(packet_size & 0x7FFFu);

    if (packet_size < 4u || packet_size > len || packet_size > SH2_MAX_PACKET)
    {
        return 0;
    }

    sh2_diag_packets++;
    sh2_diag_last_hdr[0] = header[0];
    sh2_diag_last_hdr[1] = header[1];
    sh2_diag_last_hdr[2] = header[2];
    sh2_diag_last_hdr[3] = header[3];

    /* 与 Adafruit 官方实现一致：BNO085 每次 I2C 事务都从包头重新发送，
       所以第一次读满 packet_size，后续每次多读 4 字节并跳过重复的包头。 */
    uint16_t remaining = packet_size;
    bool first = true;
    uint8_t* dst = pBuffer;
    uint8_t chunk[32];

    while (remaining > 0u)
    {
        uint16_t read_size;
        if (first)
        {
            read_size = (remaining > sizeof(chunk)) ? (uint16_t)sizeof(chunk) : remaining;
        }
        else
        {
            const uint16_t want = (uint16_t)(remaining + 4u);
            read_size = (want > sizeof(chunk)) ? (uint16_t)sizeof(chunk) : want;
        }

        if (transfer(true, s_i2c_addr8, chunk, read_size, 50u) != HAL_OK)
        {
            return 0;
        }

        uint16_t cargo;
        if (first)
        {
            cargo = read_size;
            memcpy(dst, chunk, cargo);
            first = false;
        }
        else
        {
            cargo = (uint16_t)(read_size - 4u);
            memcpy(dst, chunk + 4, cargo);
        }

        dst += cargo;
        remaining = (uint16_t)(remaining - cargo);
    }

    if (packet_size >= 8u)
    {
        sh2_diag_last_pl[0] = pBuffer[4];
        sh2_diag_last_pl[1] = pBuffer[5];
        sh2_diag_last_pl[2] = pBuffer[6];
        sh2_diag_last_pl[3] = pBuffer[7];
    }

    if (t_us != NULL)
    {
        *t_us = (uint32_t)(HAL_GetTick() * 1000u);
    }
    return (int)packet_size;
}

static int hal_write(sh2_Hal_t* self, uint8_t* pBuffer, unsigned len)
{
    (void)self;
    if (len == 0u || len > SH2_HAL_MAX_TRANSFER_OUT)
    {
        return -1;
    }
    if (transfer(false, s_i2c_addr8, pBuffer, (uint16_t)len, 50u) != HAL_OK)
    {
        return -1;
    }
    return (int)len;
}

sh2_Hal_t sh2_hal_stm32 = {
    .open = hal_open,
    .close = hal_close,
    .read = hal_read,
    .write = hal_write,
    .getTimeUs = hal_getTimeUs,
};
