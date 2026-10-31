/*
 * ultrasonic_config.h
 *
 * 超声波测距与安全限速的编译期配置。
 * 设计见 Docs/Ultrasonic-Safety-Design.md。
 */

#ifndef SYSTEM_COMMON_INC_ULTRASONIC_CONFIG_H_
#define SYSTEM_COMMON_INC_ULTRASONIC_CONFIG_H_

#include "stm32h7xx_hal.h"

/*------------------------------------------------------------------------------
 * 功能开关（分模块测试用）
 *---------------------------------------------------------------------------*/

/** 安全限速总开关：
 *  1 = 按距离限速/停车
 *  0 = 运动完全不受超声波影响（超声波仍正常测量、打日志、上报） */
#define SAFETY_GOVERNOR_ENABLED 1

/** 前向传感器是否启用（已安装=1） */
#define ULTRASONIC_FRONT_ENABLED 1

/** 后向传感器是否启用（2026-10-31 已安装并测试通过） */
#define ULTRASONIC_REAR_ENABLED 1

/*------------------------------------------------------------------------------
 * 安全距离参数（cm）
 *---------------------------------------------------------------------------*/

/** 停止距离：小于等于该值停车 */
#define SAFETY_STOP_CM 8

/** 开始减速距离：大于该值全速，中间线性减速 */
#define SAFETY_SLOW_CM 40

/** 安装偏移：传感器端面比车身外沿内缩时填正值 */
#define FRONT_OFFSET_CM 0
#define REAR_OFFSET_CM 0

/** 硬停止解除的迟滞距离（cm）：距离大于该值才解除短接刹车 */
#define SAFETY_BRAKE_RELEASE_CM 12

/** GOAL 模式持续被挡多久后中止（ms） */
#define SAFETY_GOAL_BLOCK_ABORT_MS 5000u

/*------------------------------------------------------------------------------
 * 引脚分配（与 Docs/HardwareWiring.md 第 7 节一致）
 *
 * Echo 走 EXTI 双边沿中断 + TIM7 1µs 自由计数，不占用复用功能，
 * 避免 AF 映射出错。
 *---------------------------------------------------------------------------*/

#define US_FRONT_TRIG_PORT GPIOB
#define US_FRONT_TRIG_PIN  GPIO_PIN_0
#define US_FRONT_ECHO_PORT GPIOA
#define US_FRONT_ECHO_PIN  GPIO_PIN_2

#define US_REAR_TRIG_PORT GPIOB
#define US_REAR_TRIG_PIN  GPIO_PIN_1
#define US_REAR_ECHO_PORT GPIOA
#define US_REAR_ECHO_PIN  GPIO_PIN_3

/** 自由运行的 1 µs 计数时基（16 位，回绕周期 65.5ms） */
#define US_TIMEBASE_INSTANCE TIM7

/*------------------------------------------------------------------------------
 * 时序参数
 *---------------------------------------------------------------------------*/

/** Trig 高电平宽度（µs），HC-SR04 要求 ≥10 µs */
#define US_TRIG_PULSE_US 10u

/** 触发后等待上升沿的超时（ms） */
#define US_RISE_TIMEOUT_MS 5u

/** 触发后等待下降沿的超时（ms）；400cm 对应约 23.2 ms */
#define US_FALL_TIMEOUT_MS 30u

/** 有效距离范围（cm） */
#define US_MIN_DISTANCE_CM 2
#define US_MAX_DISTANCE_CM 400

/** SonarTask 基础节拍（ms）；前后错开一拍 = 50ms */
#define US_TASK_PERIOD_MS 50u

/** 上报周期：每 N 拍发一帧（4 拍 = 200ms = 5Hz） */
#define US_REPORT_DIVIDER 4u

/** 状态日志周期：每 N 拍一条（20 拍 = 1s） */
#define US_LOG_DIVIDER 20u

#endif /* SYSTEM_COMMON_INC_ULTRASONIC_CONFIG_H_ */
