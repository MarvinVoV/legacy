/*
 * display.h
 *
 * GC9A01 圆形 IPS 屏驱动（SPI2 + DC/CS/RST GPIO）。
 * 阶段 1 只提供初始化与整屏填充，用于验证接线。
 */

#ifndef MODULES_DISPLAY_INC_DISPLAY_H_
#define MODULES_DISPLAY_INC_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

/* RGB565 常用颜色 */
#define DISPLAY_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))
#define DISPLAY_COLOR_RED   0xF800u
#define DISPLAY_COLOR_GREEN 0x07E0u
#define DISPLAY_COLOR_BLUE  0x001Fu
#define DISPLAY_COLOR_BLACK 0x0000u
#define DISPLAY_COLOR_WHITE 0xFFFFu

/**
 * @brief 初始化 SPI2、控制脚并执行 GC9A01 初始化序列
 * @return true 成功；false 表示 SPI/GPIO 初始化失败
 */
bool Display_Init(void);

/** 整屏填充单一颜色（RGB565） */
void Display_Fill(uint16_t color);

/** 矩形填充；超出屏幕的部分自动裁剪 */
void Display_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/** 上电自检：红 → 绿 → 蓝 → 停在蓝色（阻塞，仅启动阶段调用） */
void Display_SelfTest(void);

/**
 * @brief 诊断：每 1.5 秒切换 display off(0x28) / on(0x29)，共 5 轮。
 *
 * 用于确认命令是否真的到达屏幕：若能看到屏幕明暗变化，说明 SPI 与
 * DC/CS 通路正常，问题在数据格式；若毫无变化，说明命令根本没到屏。
 */
void Display_DiagToggle(void);

#endif /* MODULES_DISPLAY_INC_DISPLAY_H_ */
