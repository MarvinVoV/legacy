/*
 * ultrasonic.c
 *
 * HC-SR04 驱动实现。
 */

#include "ultrasonic.h"

#include <stddef.h>

#include "ultrasonic_config.h"

/** 声速换算：58 µs/cm（20 °C，343 m/s） */
#define US_US_PER_CM 58u

/**
 * @brief DWT 周期计数器微秒延时
 *
 * 供 Trig 脉冲使用；只在任务上下文调用，10 µs 级忙等可接受。
 */
static void dwt_delay_us(uint32_t us)
{
    const uint32_t start  = DWT->CYCCNT;
    const uint32_t cycles = us * (SystemCoreClock / 1000000u);
    while ((DWT->CYCCNT - start) < cycles)
    {
        /* busy wait */
    }
}

static int16_t width_to_cm(uint16_t widthUs)
{
    const uint32_t minUs = (uint32_t)US_MIN_DISTANCE_CM * US_US_PER_CM;
    const uint32_t maxUs = (uint32_t)US_MAX_DISTANCE_CM * US_US_PER_CM;

    /* 太近/噪声脉冲：无效 */
    if (widthUs < minUs)
    {
        return -1;
    }

    /* 超出量程：HC-SR04 无回波时输出约 38ms 长脉冲（远超 400cm），
       应视为"远端畅通"而不是故障，否则空旷环境会被判成传感器故障。 */
    if (widthUs > maxUs)
    {
        return (int16_t)US_MAX_DISTANCE_CM;
    }
    return (int16_t)(((uint32_t)widthUs + US_US_PER_CM / 2u) / US_US_PER_CM);
}

void Ultrasonic_Init(UltrasonicSensor* s, const UltrasonicHw* hw)
{
    if (s == NULL || hw == NULL)
    {
        return;
    }

    s->hw          = *hw;
    s->riseTick    = 0u;
    s->phase       = US_PHASE_IDLE;
    s->result      = 0u;
    s->resultValid = 0u;
    s->resultCm    = -1;
    s->triggerMs   = 0u;
}

void Ultrasonic_Start(UltrasonicSensor* s, const uint32_t nowMs)
{
    if (s == NULL)
    {
        return;
    }

    /* 先进入等待上升沿状态，避免脉冲期间漏掉边沿 */
    s->riseTick    = 0u;
    s->triggerMs   = nowMs;
    s->result      = 0u;
    s->resultValid = 0u;
    s->phase       = US_PHASE_WAIT_RISE;

    HAL_GPIO_WritePin(s->hw.trigPort, s->hw.trigPin, GPIO_PIN_SET);
    dwt_delay_us(US_TRIG_PULSE_US);
    HAL_GPIO_WritePin(s->hw.trigPort, s->hw.trigPin, GPIO_PIN_RESET);
}

bool Ultrasonic_Poll(UltrasonicSensor* s, const uint32_t nowMs, int16_t* distanceCm)
{
    if (s == NULL)
    {
        return false;
    }

    /* 超时判定：两种超时都按"超出量程 = 畅通"处理。
     *
     * 实测该模块在没有回波时**不输出任何脉冲**（不是 datasheet 说的 38ms），
     * 所以"没有上升沿"与"传感器断线"在电气上无法区分。若把无回波判成故障，
     * 空旷环境下该方向会被永久禁行。因此统一视为远端畅通（400cm）；
     * "从未测到过有效值"的情形由安全层的 everValid 规则放行/告警。 */
    if (s->phase == US_PHASE_WAIT_RISE &&
        (nowMs - s->triggerMs) >= US_RISE_TIMEOUT_MS)
    {
        s->phase       = US_PHASE_IDLE;
        s->result      = 1u;
        s->resultValid = 1u;
        s->resultCm    = US_MAX_DISTANCE_CM;
    }
    else if (s->phase == US_PHASE_WAIT_FALL &&
             (nowMs - s->triggerMs) >= US_FALL_TIMEOUT_MS)
    {
        s->phase       = US_PHASE_IDLE;
        s->result      = 1u;
        s->resultValid = 1u;
        s->resultCm    = US_MAX_DISTANCE_CM;
    }

    if (!s->result)
    {
        return false;
    }

    s->result = 0u;
    if (distanceCm != NULL)
    {
        *distanceCm = s->resultValid ? s->resultCm : -1;
    }
    return true;
}

void Ultrasonic_OnEdge(UltrasonicSensor* s, const bool edgeHigh)
{
    if (s == NULL)
    {
        return;
    }

    const uint16_t now = (uint16_t)s->hw.timebase->CNT;

    if (edgeHigh)
    {
        if (s->phase == US_PHASE_WAIT_RISE)
        {
            s->riseTick = now;
            s->phase    = US_PHASE_WAIT_FALL;
        }
        return;
    }

    /* 下降沿：只有等到过上升沿才有效 */
    if (s->phase != US_PHASE_WAIT_FALL)
    {
        return;
    }

    const uint16_t widthUs = (uint16_t)(now - s->riseTick);
    s->phase               = US_PHASE_IDLE;
    s->resultCm            = width_to_cm(widthUs);
    s->resultValid         = (s->resultCm > 0) ? 1u : 0u;
    s->result              = 1u;
}
