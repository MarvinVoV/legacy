/*
 * buzzer_config.h
 *
 * 蜂鸣器配置（有源蜂鸣器：GPIO 电平控制响/不响）。
 */

#ifndef SYSTEM_COMMON_INC_BUZZER_CONFIG_H_
#define SYSTEM_COMMON_INC_BUZZER_CONFIG_H_

#include "stm32h7xx_hal.h"

/** 1 = 启用蜂鸣器（0 = 完全不驱动，便于对比） */
#define BUZZER_ENABLED 1

/** 蜂鸣器引脚：PB2（空闲） */
#define BUZZER_PORT GPIOB
#define BUZZER_PIN  GPIO_PIN_2

/** 高电平响（有源蜂鸣器正极接 GPIO、负极接 GND） */
#define BUZZER_ACTIVE_HIGH 1

/** 上电自检：响 3 声（调试接线用，正式使用可置 0） */
#define BUZZER_SELF_TEST 1

/** 1 = 按距离蜂鸣（减速区 8~40cm），0 = 只保留上电自检 */
#define BUZZER_DISTANCE_BEEP 1

/** 减速区蜂鸣阈值（cm）：8 < d < 40 时响 */
#define BUZZER_NEAR_CM 40
#define BUZZER_STOP_CM 8

/** 节奏分档阈值（cm）：越近越快 */
#define BUZZER_MID_CM  25
#define BUZZER_FAST_CM 15

#endif /* SYSTEM_COMMON_INC_BUZZER_CONFIG_H_ */
