/*
 * buzzer.h
 *
 * 有源蜂鸣器驱动：GPIO 电平控制，无音调/频率控制。
 */

#ifndef MODULES_BUZZER_INC_BUZZER_H_
#define MODULES_BUZZER_INC_BUZZER_H_

#include <stdbool.h>
#include <stdint.h>

/** 初始化引脚为推挽输出并关闭蜂鸣器 */
void Buzzer_Init(void);

/** 响/不响 */
void Buzzer_Set(bool on);

/**
 * @brief 上电自检：响 count 声（阻塞，只能在启动阶段调用）
 * @param on_ms  每声持续毫秒
 * @param off_ms 每声之间静音毫秒
 */
void Buzzer_SelfTest(uint32_t on_ms, uint32_t off_ms, uint8_t count);

/**
 * @brief 按距离更新蜂鸣器（有源蜂鸣器只能改节奏，不能改音调）
 *
 * 距离越近节奏越快：
 *   d > 40cm        静音
 *   25~40cm         100ms 响 / 500ms 停
 *   15~25cm         100ms 响 / 200ms 停
 *   8~15cm          80ms 响 / 70ms 停
 *   ≤ 8cm           连续响
 *
 * @param now_ms      当前毫秒（HAL_GetTick）
 * @param distance_cm 到障碍物的距离（<0 表示无效）
 */
void Buzzer_Update(uint32_t now_ms, int16_t distance_cm);

#endif /* MODULES_BUZZER_INC_BUZZER_H_ */
