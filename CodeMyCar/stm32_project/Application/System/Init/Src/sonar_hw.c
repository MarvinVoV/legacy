/*
 * sonar_hw.c
 *
 * 超声波硬件初始化：
 *  - PB0/PB1 Trig（推挽输出）
 *  - PA2/PA3 Echo（EXTI2/EXTI3 双边沿，内部下拉）
 *  - TIM7 1 µs 自由运行计数
 *  - DWT 周期计数器（Trig 微秒延时）
 *
 * 说明：本文件定义 EXTI2/EXTI3_IRQHandler 和 HAL_GPIO_EXTI_Callback，
 * 启动文件里它们是 weak 符号，业务文件定义强符号即可覆盖，
 * 且不会因为 CubeMX 重新生成 Core/ 而丢失。
 */

#include "sonar_hw.h"

#include <stddef.h>

#include "ultrasonic_config.h"

/*------------------------------------------------------------------------------
 * 实例
 *---------------------------------------------------------------------------*/

static UltrasonicSensor s_front;
static UltrasonicSensor s_rear;

static TIM_HandleTypeDef s_htim7;

/*------------------------------------------------------------------------------
 * 工具
 *---------------------------------------------------------------------------*/

/** APB1 定时器时钟：APB1 分频不为 1 时，定时器时钟 = 2 × PCLK1 */
static uint32_t apb1_timer_clock_hz(void)
{
    const uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    const uint32_t ppre1 = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) >> RCC_D2CFGR_D2PPRE1_Pos;
    return (ppre1 < 4u) ? pclk1 : pclk1 * 2u;
}

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void timebase_init(void)
{
    __HAL_RCC_TIM7_CLK_ENABLE();

    const uint32_t timclk = apb1_timer_clock_hz();
    const uint32_t prescaler = (timclk / 1000000u > 0u) ? (timclk / 1000000u - 1u) : 0u;

    s_htim7.Instance               = US_TIMEBASE_INSTANCE;
    s_htim7.Init.Prescaler         = prescaler;
    s_htim7.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim7.Init.Period            = 0xFFFFu;
    s_htim7.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_Base_Init(&s_htim7) == HAL_OK)
    {
        HAL_TIM_Base_Start(&s_htim7);
    }
}

static void gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* Trig：推挽输出，初始低 */
    gpio.Pin   = US_FRONT_TRIG_PIN | US_REAR_TRIG_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(US_FRONT_TRIG_PORT, US_FRONT_TRIG_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(US_REAR_TRIG_PORT, US_REAR_TRIG_PIN, GPIO_PIN_RESET);

    /* Echo：双边沿中断输入，内部下拉（未接传感器时保持低电平） */
    gpio.Pin  = US_FRONT_ECHO_PIN | US_REAR_ECHO_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* EXTI2/EXTI3 映射到 PA（复位后默认就是 PA，这里显式设置更稳妥） */
    SYSCFG->EXTICR[0] &= ~((0xFu << 8) | (0xFu << 12));

    HAL_NVIC_SetPriority(EXTI2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI2_IRQn);
    HAL_NVIC_SetPriority(EXTI3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);
}

/*------------------------------------------------------------------------------
 * 公共 API
 *---------------------------------------------------------------------------*/

void SonarHw_Init(void)
{
    dwt_init();
    gpio_init();
    timebase_init();

    const UltrasonicHw frontHw = {
        .trigPort = US_FRONT_TRIG_PORT,
        .trigPin  = US_FRONT_TRIG_PIN,
        .echoPort = US_FRONT_ECHO_PORT,
        .echoPin  = US_FRONT_ECHO_PIN,
        .timebase = US_TIMEBASE_INSTANCE,
    };
    const UltrasonicHw rearHw = {
        .trigPort = US_REAR_TRIG_PORT,
        .trigPin  = US_REAR_TRIG_PIN,
        .echoPort = US_REAR_ECHO_PORT,
        .echoPin  = US_REAR_ECHO_PIN,
        .timebase = US_TIMEBASE_INSTANCE,
    };

    Ultrasonic_Init(&s_front, &frontHw);
    Ultrasonic_Init(&s_rear, &rearHw);
}

UltrasonicSensor* SonarHw_Front(void)
{
    return &s_front;
}

UltrasonicSensor* SonarHw_Rear(void)
{
    return &s_rear;
}

/*------------------------------------------------------------------------------
 * EXTI
 *---------------------------------------------------------------------------*/

void HAL_GPIO_EXTI_Callback(const uint16_t GPIO_Pin)
{
    if (GPIO_Pin == US_FRONT_ECHO_PIN)
    {
        Ultrasonic_OnEdge(&s_front,
                          HAL_GPIO_ReadPin(US_FRONT_ECHO_PORT, US_FRONT_ECHO_PIN) == GPIO_PIN_SET);
    }
    else if (GPIO_Pin == US_REAR_ECHO_PIN)
    {
        Ultrasonic_OnEdge(&s_rear,
                          HAL_GPIO_ReadPin(US_REAR_ECHO_PORT, US_REAR_ECHO_PIN) == GPIO_PIN_SET);
    }
}

void EXTI2_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(US_FRONT_ECHO_PIN);
}

void EXTI3_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(US_REAR_ECHO_PIN);
}
