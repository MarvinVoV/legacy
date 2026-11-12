#include "led_strip.h"
#include "stm32h7xx_hal.h"

/* Dedicated TIM1/PA8 and DMA2_Stream0; no shared motion timer or DMA stream. */
static TIM_HandleTypeDef s_timer;
static DMA_HandleTypeDef s_dma;
static Ws2812Timing s_timing;
static uint32_t s_words[WS2812_BUFFER_WORDS]
    __attribute__((section(".dma_buffer"), aligned(32)));
static volatile bool s_busy;
static bool s_ready;
static volatile uint32_t s_completed, s_errors;

static void finish(DMA_HandleTypeDef *dma)
{
    (void)dma;
    __HAL_TIM_DISABLE_DMA(&s_timer, TIM_DMA_UPDATE);
    __HAL_TIM_SET_COMPARE(&s_timer, TIM_CHANNEL_1, 0);
    (void)HAL_TIM_PWM_Stop(&s_timer, TIM_CHANNEL_1);
    ++s_completed;
    s_busy = false;
}
static void fail(DMA_HandleTypeDef *dma)
{
    (void)dma;
    __HAL_TIM_DISABLE_DMA(&s_timer, TIM_DMA_UPDATE);
    __HAL_TIM_SET_COMPARE(&s_timer, TIM_CHANNEL_1, 0);
    (void)HAL_TIM_PWM_Stop(&s_timer, TIM_CHANNEL_1);
    ++s_errors;
    s_busy = false;
}
bool LedStrip_Init(void)
{
    if (s_ready) return true;
    /* TIMPRE=0: divided APB timer kernels run at twice PCLK. Do not change a
     * shared clock source underneath existing motor/IMU peripherals. */
    if (RCC->CFGR & RCC_CFGR_TIMPRE) return false;
    const uint32_t pclk = HAL_RCC_GetPCLK2Freq();
    const uint32_t hz = pclk == HAL_RCC_GetHCLKFreq() ? pclk : pclk * 2u;
    if (!Ws2812_Timing(hz, &s_timing)) return false;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_8;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_PULLDOWN;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    pin.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOA, &pin);

    s_timer.Instance = TIM1;
    s_timer.Init.Prescaler = 0;
    s_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    s_timer.Init.Period = s_timing.periodTicks - 1u;
    s_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    s_timer.Init.RepetitionCounter = 0;
    s_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&s_timer) != HAL_OK) return false;
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&s_timer, &oc, TIM_CHANNEL_1) != HAL_OK) return false;
    TIM_BreakDeadTimeConfigTypeDef idle = {0};
    idle.OffStateRunMode = TIM_OSSR_ENABLE;
    idle.OffStateIDLEMode = TIM_OSSI_ENABLE;
    idle.LockLevel = TIM_LOCKLEVEL_OFF;
    idle.BreakState = TIM_BREAK_DISABLE;
    idle.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
    idle.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
    if (HAL_TIMEx_ConfigBreakDeadTime(&s_timer, &idle) != HAL_OK) return false;

    s_dma.Instance = DMA2_Stream0;
    s_dma.Init.Request = DMA_REQUEST_TIM1_UP;
    s_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    s_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    s_dma.Init.MemInc = DMA_MINC_ENABLE;
    s_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    s_dma.Init.Mode = DMA_NORMAL;
    s_dma.Init.Priority = DMA_PRIORITY_LOW;
    s_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_dma) != HAL_OK) return false;
    s_dma.XferCpltCallback = finish;
    s_dma.XferErrorCallback = fail;
    s_dma.XferHalfCpltCallback = NULL;
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 6, 0);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    s_ready = true;
    return true;
}
bool LedStrip_Show(const LedRgb pixels[WS2812_LED_COUNT])
{
    if (!s_ready || s_busy) return false;
    const size_t count = Ws2812_Encode(pixels, &s_timing, s_words, WS2812_BUFFER_WORDS);
    if (!count) return false;
    /* .dma_buffer is MPU non-cacheable, shared with DMA2. */
    __DMB();
    s_busy = true;
    /* CCR preload gives two initial low periods. Every update then latches the
     * preceding DMA word; the first payload bit is never skipped or repeated. */
    __HAL_TIM_DISABLE_DMA(&s_timer, TIM_DMA_UPDATE);
    __HAL_TIM_SET_COUNTER(&s_timer, 0);
    __HAL_TIM_SET_COMPARE(&s_timer, TIM_CHANNEL_1, 0);
    s_timer.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&s_timer, TIM_FLAG_UPDATE);
    if (HAL_DMA_Start_IT(&s_dma, (uint32_t)s_words, (uint32_t)&TIM1->CCR1, count) != HAL_OK) {
        ++s_errors; s_busy = false; return false;
    }
    __HAL_TIM_ENABLE_DMA(&s_timer, TIM_DMA_UPDATE);
    if (HAL_TIM_PWM_Start(&s_timer, TIM_CHANNEL_1) != HAL_OK) {
        __HAL_TIM_DISABLE_DMA(&s_timer, TIM_DMA_UPDATE);
        (void)HAL_DMA_Abort(&s_dma);
        ++s_errors; s_busy = false; return false;
    }
    return true;
}
void LedStrip_DmaIRQHandler(void) { HAL_DMA_IRQHandler(&s_dma); }
uint32_t LedStrip_CompletedFrames(void) { return s_completed; }
uint32_t LedStrip_ErrorCount(void) { return s_errors; }
