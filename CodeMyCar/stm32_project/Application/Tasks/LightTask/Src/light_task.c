#include "light_task.h"
#include "led_strip.h"
#include "lighting_service.h"
#include "cmsis_os2.h"
#include "freertos_os2.h"
#include "stm32h7xx_hal.h"
#include "log_manager.h"

static void light_run(void *arg)
{
    (void)arg;
    if (!LedStrip_Init()) {
        LOG_ERROR(LOG_MODULE_SYSTEM,"LED init failed; vehicle control remains independent");
        osThreadExit();
        return;
    }
    LOG_INFO(LOG_MODULE_SYSTEM,"LED ready PA8 TIM1 DMA2S0 GRB six pixels max=12/255");
    const uint32_t start = HAL_GetTick();
    uint32_t lastLog = start;
    for (;;) {
        const uint32_t wake = osKernelGetTickCount();
        LedRgb pixels[WS2812_LED_COUNT];
        Lighting_RenderBringup(HAL_GetTick()-start,pixels);
        (void)LedStrip_Show(pixels);
        if (HAL_GetTick()-lastLog >= 5000u) {
            lastLog = HAL_GetTick();
            LOG_INFO(LOG_MODULE_SYSTEM,"LED frames=%lu errors=%lu",
                     (unsigned long)LedStrip_CompletedFrames(),
                     (unsigned long)LedStrip_ErrorCount());
        }
        osDelayUntil(wake+pdMS_TO_TICKS(40));
    }
}
void LightTask_Init(void)
{
    const osThreadAttr_t attrs={.name="LightTask",.stack_size=1536,.priority=osPriorityLow};
    if (!osThreadNew(light_run,NULL,&attrs))
        LOG_ERROR(LOG_MODULE_SYSTEM,"Failed to create LightTask");
}
