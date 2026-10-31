/*
 * buzzer.c
 *
 * 有源蜂鸣器驱动实现。
 */

#include "buzzer.h"

#include "buzzer_config.h"

static void buzzer_write(bool on)
{
#if BUZZER_ACTIVE_HIGH
    const GPIO_PinState level = on ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
    const GPIO_PinState level = on ? GPIO_PIN_RESET : GPIO_PIN_SET;
#endif
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, level);
}

void Buzzer_Init(void)
{
#if BUZZER_ENABLED
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = BUZZER_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BUZZER_PORT, &gpio);

    Buzzer_Set(false);
#endif
}

void Buzzer_Set(const bool on)
{
#if BUZZER_ENABLED
    buzzer_write(on);
#else
    (void)on;
#endif
}

/**
 * @brief DWT 周期计数器延时
 *
 * 启动阶段不能依赖 HAL_Delay：这里 HAL 时基（TIM6）尚未可靠运行，
 * 实测 uwTick 恒为 0，HAL_Delay 会永久阻塞。
 */
static void delay_ms_dwt(uint32_t ms)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u)
    {
        DWT->CYCCNT = 0u;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    const uint32_t cycles = ms * (SystemCoreClock / 1000u);
    const uint32_t start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles)
    {
        /* busy wait */
    }
}

void Buzzer_Update(const uint32_t now_ms, const int16_t distance_cm)
{
#if BUZZER_ENABLED && BUZZER_DISTANCE_BEEP
    /* 无效距离 / 超出减速区 → 静音 */
    if (distance_cm <= 0 || distance_cm > BUZZER_NEAR_CM)
    {
        Buzzer_Set(false);
        return;
    }

    /* 停止区：连续响 */
    if (distance_cm <= BUZZER_STOP_CM)
    {
        Buzzer_Set(true);
        return;
    }

    /* 减速区：距离越近，周期越短 */
    uint32_t period_ms;
    uint32_t on_ms;

    if (distance_cm > BUZZER_MID_CM)
    {
        period_ms = 600u; /* 25~40cm：滴 100ms / 停 500ms */
        on_ms     = 100u;
    }
    else if (distance_cm > BUZZER_FAST_CM)
    {
        period_ms = 300u; /* 15~25cm：滴 100ms / 停 200ms */
        on_ms     = 100u;
    }
    else
    {
        period_ms = 150u; /* 8~15cm：滴 80ms / 停 70ms */
        on_ms     = 80u;
    }

    Buzzer_Set((now_ms % period_ms) < on_ms);
#else
    (void)now_ms;
    (void)distance_cm;
#endif
}

void Buzzer_SelfTest(const uint32_t on_ms, const uint32_t off_ms, const uint8_t count)
{
#if BUZZER_ENABLED
    for (uint8_t i = 0; i < count; i++)
    {
        Buzzer_Set(true);
        delay_ms_dwt(on_ms);
        Buzzer_Set(false);
        if (i + 1u < count)
        {
            delay_ms_dwt(off_ms);
        }
    }
#else
    (void)on_ms;
    (void)off_ms;
    (void)count;
#endif
}
