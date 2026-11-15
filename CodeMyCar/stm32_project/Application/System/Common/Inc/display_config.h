/*
 * display_config.h
 *
 * GC9A01 1.28" 240x240 圆形 IPS 屏（SPI）的编译期配置。
 * 接线记录见 Docs/HardwareWiring.md 第 9 节。
 */

#ifndef SYSTEM_COMMON_INC_DISPLAY_CONFIG_H_
#define SYSTEM_COMMON_INC_DISPLAY_CONFIG_H_

/* 总开关：关闭后不初始化 SPI/GPIO，不占用任何引脚 */
#define DISPLAY_ENABLED 1

#define DISPLAY_WIDTH  240u
#define DISPLAY_HEIGHT 240u

/* SPI2：PB13 = SCK，PB15 = MOSI（AF5） */
#define DISPLAY_SPI_INSTANCE     SPI2
#define DISPLAY_SPI_FORCE_RESET() __HAL_RCC_SPI2_FORCE_RESET()
#define DISPLAY_SPI_RELEASE_RESET() __HAL_RCC_SPI2_RELEASE_RESET()

#define DISPLAY_SCK_PORT   GPIOB
#define DISPLAY_SCK_PIN    GPIO_PIN_13
#define DISPLAY_MOSI_PORT  GPIOB
#define DISPLAY_MOSI_PIN   GPIO_PIN_15
#define DISPLAY_GPIO_AF    GPIO_AF5_SPI2

/* 控制脚 */
#define DISPLAY_DC_PORT    GPIOB
#define DISPLAY_DC_PIN     GPIO_PIN_9
#define DISPLAY_CS_PORT    GPIOB
#define DISPLAY_CS_PIN     GPIO_PIN_12
#define DISPLAY_RST_PORT   GPIOB
#define DISPLAY_RST_PIN    GPIO_PIN_14

/* SPI kernel = 125 MHz，/16 ≈ 7.8 MHz；保留已实测稳定的线速。 */
#define DISPLAY_SPI_PRESCALER  SPI_BAUDRATEPRESCALER_16

/* 像素色深：16 或 18。GC9A01 复位默认 18 位，若 0x3A=0x05 未生效
 * 会出现固定周期的彩色竖条纹，此时改用 18 位逐像素 3 字节发送。 */
#define DISPLAY_BITS_PER_PIXEL 16

/* 上电自检：红 → 绿 → 蓝 → 停在蓝色 */
#define DISPLAY_SELF_TEST 1
#define DISPLAY_SELF_TEST_HOLD_MS 2000u

#endif /* SYSTEM_COMMON_INC_DISPLAY_CONFIG_H_ */
