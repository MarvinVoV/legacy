/*
 * display.c
 *
 * GC9A01 240x240 圆形 IPS 屏驱动实现。
 *
 * 保留已验证的初始化和 7.8MHz 线速；支持整屏填充与局部帧缓冲传输。
 */

#include "display.h"
#include <stddef.h>

#include "display_config.h"
#include "cmsis_os2.h"
#include "stm32h7xx_hal.h"

#if DISPLAY_ENABLED

static SPI_HandleTypeDef s_spi;

/* The display task is the sole owner. Stop issuing SPI transfers after the
 * first error in each register sequence or frame window. */
static bool s_command_ok;
enum { SPI_TIMEOUT_MS = 100 };

static void display_delay_ms(uint32_t ms)
{
    osDelay((ms * osKernelGetTickFreq() + 999u) / 1000u);
}

/* ==================== 片选 / 数据指令 ==================== */

static void cs_low(void)  { HAL_GPIO_WritePin(DISPLAY_CS_PORT, DISPLAY_CS_PIN, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(DISPLAY_CS_PORT, DISPLAY_CS_PIN, GPIO_PIN_SET); }

/* 商家例程对每个字节都切换 CS，本模块对 CS 行为敏感，照此实现。 */
static void write_byte(const uint8_t value)
{
    if (!s_command_ok) return;
    cs_low();
    s_command_ok = HAL_SPI_Transmit(&s_spi, (uint8_t *)&value, 1u, SPI_TIMEOUT_MS) == HAL_OK;
    cs_high();
}

static void write_cmd(const uint8_t cmd)
{
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_RESET);
    write_byte(cmd);
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_SET);
}

static void write_data(const uint8_t *data, uint16_t len)
{
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_SET);
    for (uint16_t i = 0u; i < len; ++i)
    {
        write_byte(data[i]);
    }
}

static void write_u8(const uint8_t value)
{
    write_data(&value, 1u);
}

/* ==================== 硬件初始化 ==================== */

static void display_gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* SCK / MOSI 复用 */
    GPIO_InitTypeDef af = {0};
    af.Pin       = DISPLAY_SCK_PIN | DISPLAY_MOSI_PIN;
    af.Mode      = GPIO_MODE_AF_PP;
    af.Pull      = GPIO_NOPULL;
    af.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    af.Alternate = DISPLAY_GPIO_AF;
    HAL_GPIO_Init(GPIOB, &af);

    /* DC / CS / RST 推挽输出 */
    GPIO_InitTypeDef out = {0};
    out.Pin   = DISPLAY_DC_PIN | DISPLAY_CS_PIN | DISPLAY_RST_PIN;
    out.Mode  = GPIO_MODE_OUTPUT_PP;
    out.Pull  = GPIO_NOPULL;
    out.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &out);

    cs_high();
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISPLAY_RST_PORT, DISPLAY_RST_PIN, GPIO_PIN_SET);
}

static bool display_spi_init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();

    s_spi.Instance               = DISPLAY_SPI_INSTANCE;
    s_spi.Init.Mode              = SPI_MODE_MASTER;
    s_spi.Init.Direction         = SPI_DIRECTION_2LINES;
    s_spi.Init.DataSize          = SPI_DATASIZE_8BIT;
    s_spi.Init.CLKPolarity       = SPI_POLARITY_LOW;
    s_spi.Init.CLKPhase          = SPI_PHASE_1EDGE;
    s_spi.Init.NSS               = SPI_NSS_SOFT;
    s_spi.Init.BaudRatePrescaler = DISPLAY_SPI_PRESCALER;
    s_spi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    s_spi.Init.TIMode            = SPI_TIMODE_DISABLE;
    s_spi.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    s_spi.Init.CRCPolynomial     = 0u;
    s_spi.Init.FifoThreshold     = SPI_FIFO_THRESHOLD_01DATA;

    return HAL_SPI_Init(&s_spi) == HAL_OK;
}

static void display_hw_reset(void)
{
    /* 与商家例程一致：低 100ms -> 高 100ms */
    HAL_GPIO_WritePin(DISPLAY_RST_PORT, DISPLAY_RST_PIN, GPIO_PIN_RESET);
    display_delay_ms(100u);
    HAL_GPIO_WritePin(DISPLAY_RST_PORT, DISPLAY_RST_PIN, GPIO_PIN_SET);
    display_delay_ms(100u);
}

/* ==================== GC9A01 初始化序列 ==================== */

static void gc9a01_init_sequence(void)
{
    /* 睡眠状态下大部分寄存器被锁定：必须先 Sleep Out，再发厂商寄存器，
     * 否则 0x3A/0x36 会被静默忽略（表现为黑屏或 18 位色竖线条纹）。 */
    write_cmd(0x11);
    display_delay_ms(120u);

    write_cmd(0xEF);
    write_u8(0xEB);
    write_u8(0x14);

    write_cmd(0xEB);
    write_u8(0x14);

    write_cmd(0xFE);
    write_cmd(0xEF);
    write_cmd(0xEB);
    write_u8(0x14);

    write_cmd(0x84); write_u8(0x40);
    write_cmd(0x85); write_u8(0xFF);
    write_cmd(0x86); write_u8(0xFF);
    write_cmd(0x87); write_u8(0xFF);
    write_cmd(0x88); write_u8(0x0A);
    write_cmd(0x89); write_u8(0x21);
    write_cmd(0x8A); write_u8(0x00);
    write_cmd(0x8B); write_u8(0x80);
    write_cmd(0x8C); write_u8(0x01);
    write_cmd(0x8D); write_u8(0x01);
    write_cmd(0x8E); write_u8(0xFF);
    write_cmd(0x8F); write_u8(0xFF);

    write_cmd(0xB6); write_u8(0x00); write_u8(0x20);

    /* MADCTL：后续做眼睛动画时按实际装配方向调整 */
    write_cmd(0x36); write_u8(0x08);
    /* COLMOD：16 bit/pixel */
    #if DISPLAY_BITS_PER_PIXEL == 18
    write_cmd(0x3A); write_u8(0x06);
#else
    write_cmd(0x3A); write_u8(0x05);
#endif

    write_cmd(0x90);
    write_u8(0x08); write_u8(0x08); write_u8(0x08); write_u8(0x08);

    write_cmd(0xBD); write_u8(0x06);
    write_cmd(0xBC); write_u8(0x00);

    write_cmd(0xFF);
    write_u8(0x60); write_u8(0x01); write_u8(0x04);

    write_cmd(0xC3); write_u8(0x13);
    write_cmd(0xC4); write_u8(0x13);
    write_cmd(0xC9); write_u8(0x22);
    write_cmd(0xBE); write_u8(0x11);

    write_cmd(0xE1); write_u8(0x10); write_u8(0x0E);

    write_cmd(0xDF);
    write_u8(0x21); write_u8(0x0C); write_u8(0x02);

    write_cmd(0xF0);
    write_u8(0x45); write_u8(0x09); write_u8(0x08); write_u8(0x08);
    write_u8(0x26); write_u8(0x2A);

    write_cmd(0xF1);
    write_u8(0x43); write_u8(0x70); write_u8(0x72); write_u8(0x36);
    write_u8(0x37);

    write_cmd(0xF2);
    write_u8(0x45); write_u8(0x09); write_u8(0x08); write_u8(0x08);
    write_u8(0x26); write_u8(0x2A);

    write_cmd(0xF3);
    write_u8(0x43); write_u8(0x70); write_u8(0x72); write_u8(0x36);
    write_u8(0x37);

    write_cmd(0xED);
    write_u8(0x1B); write_u8(0x0B);

    write_cmd(0xAE); write_u8(0x77);
    write_cmd(0xCD); write_u8(0x63);

    write_cmd(0x70);
    write_u8(0x07); write_u8(0x07); write_u8(0x04); write_u8(0x0E);
    write_u8(0x0F); write_u8(0x09); write_u8(0x07); write_u8(0x08);
    write_u8(0x03);

    write_cmd(0xE8); write_u8(0x34);

    write_cmd(0x62);
    write_u8(0x18); write_u8(0x0D); write_u8(0x71); write_u8(0xED);
    write_u8(0x70); write_u8(0x70); write_u8(0x18); write_u8(0x0F);
    write_u8(0x71); write_u8(0xEF); write_u8(0x70); write_u8(0x70);

    write_cmd(0x63);
    write_u8(0x18); write_u8(0x11); write_u8(0x71); write_u8(0xF1);
    write_u8(0x70); write_u8(0x70); write_u8(0x18); write_u8(0x13);
    write_u8(0x71); write_u8(0xF3); write_u8(0x70); write_u8(0x70);

    write_cmd(0x64);
    write_u8(0x28); write_u8(0x29); write_u8(0xF1); write_u8(0x01);
    write_u8(0xF1); write_u8(0x00); write_u8(0x07);

    write_cmd(0x66);
    write_u8(0x3C); write_u8(0x00); write_u8(0xCD); write_u8(0x67);
    write_u8(0x45); write_u8(0x45); write_u8(0x10); write_u8(0x00);
    write_u8(0x00); write_u8(0x00);

    write_cmd(0x67);
    write_u8(0x00); write_u8(0x3C); write_u8(0x00); write_u8(0x00);
    write_u8(0x00); write_u8(0x01); write_u8(0x54); write_u8(0x10);
    write_u8(0x32); write_u8(0x98);

    write_cmd(0x74);
    write_u8(0x10); write_u8(0x85); write_u8(0x80); write_u8(0x00);
    write_u8(0x00); write_u8(0x4E); write_u8(0x00);

    write_cmd(0x98);
    write_u8(0x3E); write_u8(0x07);

    write_cmd(0x35); /* TE on */
    write_cmd(0x21); /* 反显 */
    write_cmd(0x29); /* Display on */
    display_delay_ms(120u);
}

/* ==================== 对外接口 ==================== */

bool Display_Init(void)
{
    s_command_ok = true;
    display_gpio_init();
    if (!display_spi_init())
    {
        return false;
    }
    display_hw_reset();
    /* 初始化命令必须在片选有效期间发送；此前漏了 cs_low()，
     * 面板会忽略整段初始化序列，只显示未初始化的 GRAM 噪声。 */
    gc9a01_init_sequence();
    return s_command_ok;
}

static bool display_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    s_command_ok = true;
    write_cmd(0x2A);
    write_u8((uint8_t)(x >> 8)); write_u8((uint8_t)(x & 0xFFu));
    write_u8((uint8_t)((x + w - 1u) >> 8)); write_u8((uint8_t)((x + w - 1u) & 0xFFu));

    write_cmd(0x2B);
    write_u8((uint8_t)(y >> 8)); write_u8((uint8_t)(y & 0xFFu));
    write_u8((uint8_t)((y + h - 1u) >> 8)); write_u8((uint8_t)((y + h - 1u) & 0xFFu));

    write_cmd(0x2C);
    return s_command_ok;
}

void Display_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || w == 0u || h == 0u)
    {
        return;
    }
    if ((uint32_t)x + w > DISPLAY_WIDTH)  { w = (uint16_t)(DISPLAY_WIDTH - x); }
    if ((uint32_t)y + h > DISPLAY_HEIGHT) { h = (uint16_t)(DISPLAY_HEIGHT - y); }

    if (!display_window(x, y, w, h)) return;

    /* 整行传输（CS 在整块期间保持低）：把逐字节开销降到一行一次。
     * 若真机上出现花屏，说明该模块确实要求逐字节切 CS。 */
    static uint8_t s_row[DISPLAY_WIDTH * 2u];
    const uint8_t hi = (uint8_t)(color >> 8);
    const uint8_t lo = (uint8_t)(color & 0xFFu);
    for (uint16_t i = 0u; i < w; ++i)
    {
        s_row[2u * i]      = hi;
        s_row[2u * i + 1u] = lo;
    }

    cs_low();
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_SET);
    for (uint16_t row = 0u; row < h; ++row)
    {
        if (HAL_SPI_Transmit(&s_spi, s_row, (uint16_t)(w * 2u), SPI_TIMEOUT_MS) != HAL_OK) break;
    }
    cs_high();
}

bool Display_BlitBuffer(const uint16_t *buf, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h)
{
    if (buf == NULL || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || w == 0u || h == 0u) { return false; }
    if ((uint32_t)x + w > DISPLAY_WIDTH)  { w = (uint16_t)(DISPLAY_WIDTH - x); }
    if ((uint32_t)y + h > DISPLAY_HEIGHT) { h = (uint16_t)(DISPLAY_HEIGHT - y); }

    if (!display_window(x, y, w, h)) return false;

    cs_low();
    HAL_GPIO_WritePin(DISPLAY_DC_PORT, DISPLAY_DC_PIN, GPIO_PIN_SET);
    for (uint16_t row = 0u; row < h; ++row)
    {
        const uint16_t *src = buf + (uint32_t)(y + row) * DISPLAY_WIDTH + x;
        if (HAL_SPI_Transmit(&s_spi, (uint8_t *)src, (uint16_t)(w * 2u), SPI_TIMEOUT_MS) != HAL_OK)
        {
            cs_high();
            return false;
        }
    }
    cs_high();
    return true;
}

void Display_Fill(uint16_t color)
{
    Display_FillRect(0u, 0u, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void Display_SelfTest(void)
{
    Display_Fill(DISPLAY_COLOR_RED);
    display_delay_ms(DISPLAY_SELF_TEST_HOLD_MS);
    Display_Fill(DISPLAY_COLOR_GREEN);
    display_delay_ms(DISPLAY_SELF_TEST_HOLD_MS);
    Display_Fill(DISPLAY_COLOR_BLUE);
    display_delay_ms(DISPLAY_SELF_TEST_HOLD_MS);
}


#else /* !DISPLAY_ENABLED */

bool Display_Init(void) { return true; }
void Display_Fill(uint16_t color) { (void)color; }
void Display_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    (void)x; (void)y; (void)w; (void)h; (void)color;
}
bool Display_BlitBuffer(const uint16_t *buf, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)buf; (void)x; (void)y; (void)w; (void)h; return false;
}
void Display_SelfTest(void) { }

#endif /* DISPLAY_ENABLED */
