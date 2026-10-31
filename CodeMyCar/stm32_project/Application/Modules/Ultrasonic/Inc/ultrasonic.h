/*
 * ultrasonic.h
 *
 * HC-SR04 驱动：Trig 脉冲 + Echo 双边沿中断 + 自由计数时基。
 *
 * 线程模型：
 *  - Ultrasonic_Start() / Ultrasonic_Poll() 在 SonarTask 上下文调用
 *  - Ultrasonic_OnEdge() 在 EXTI 中断上下文调用
 *  - 单生产者/单消费者，只有 16 位/8 位原子读写，无需互斥
 */

#ifndef MODULES_ULTRASONIC_INC_ULTRASONIC_H_
#define MODULES_ULTRASONIC_INC_ULTRASONIC_H_

#include <stdbool.h>
#include <stdint.h>

#include "stm32h7xx_hal.h"

typedef struct
{
    GPIO_TypeDef* trigPort;
    uint16_t      trigPin;
    GPIO_TypeDef* echoPort;
    uint16_t      echoPin;
    TIM_TypeDef*  timebase; ///< 1 µs 自由计数定时器
} UltrasonicHw;

typedef enum
{
    US_PHASE_IDLE = 0,
    US_PHASE_WAIT_RISE,
    US_PHASE_WAIT_FALL,
} UltrasonicPhase;

typedef struct
{
    UltrasonicHw hw;

    volatile uint16_t riseTick; ///< 上升沿时刻（时基计数）
    volatile uint8_t  phase;    ///< UltrasonicPhase
    volatile uint8_t  result;   ///< 1 = 有新结果待取
    uint8_t           resultValid;
    int16_t           resultCm; ///< 新结果（无效为 -1）

    uint32_t triggerMs; ///< 本次触发时刻（HAL tick）
} UltrasonicSensor;

/** 初始化驱动实例（硬件初始化由 sonar_hw.c 负责） */
void Ultrasonic_Init(UltrasonicSensor* s, const UltrasonicHw* hw);

/** 触发一次测量（内部发 10 µs Trig 脉冲），非阻塞 */
void Ultrasonic_Start(UltrasonicSensor* s, uint32_t nowMs);

/**
 * @brief 取一次测量结果
 * @return true 表示本次调用取到了新结果（可能是有效的，也可能是 -1 无效）
 */
bool Ultrasonic_Poll(UltrasonicSensor* s, uint32_t nowMs, int16_t* distanceCm);

/** EXTI 回调：edgeHigh = 当前电平为高（上升沿） */
void Ultrasonic_OnEdge(UltrasonicSensor* s, bool edgeHigh);

#endif /* MODULES_ULTRASONIC_INC_ULTRASONIC_H_ */
