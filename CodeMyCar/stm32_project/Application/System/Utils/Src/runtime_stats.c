#include "runtime_stats.h"
#include "cycle_clock.h"
#include "platform_config.h"
#include "stm32h7xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log_manager.h"
#include "safety_supervisor.h"
#include "uart_handle.h"

#define MAX_DIAGNOSTIC_TASKS 20u
static CycleClock s_clock;
static uint32_t s_cyclesPerTick;
volatile uint32_t g_runtimeFatalReason;

void RuntimeStats_Start(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_clock = (CycleClock){.previous = DWT->CYCCNT};
    s_cyclesPerTick = SystemCoreClock / 1000000u;
}

uint64_t RuntimeStats_Counter(void)
{
    /* May be called by scheduler or task diagnostics. Preserve nested IRQ state. */
    const uint32_t mask = __get_PRIMASK();
    __disable_irq();
    const uint64_t ticks = cycle_clock_update(&s_clock, DWT->CYCCNT, s_cyclesPerTick);
    __set_PRIMASK(mask);
    return ticks;
}

void RuntimeStats_Fatal(uint32_t reason)
{
    __disable_irq();
    g_runtimeFatalReason = reason;
    /* These writes do not depend on a mutex or a functioning scheduler. */
    TIM3->CCR1 = 0u;
    TIM3->CCR2 = 0u;
    GPIOF->BSRR = (uint32_t)(GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_10) << 16u;
    __DSB();
    for (;;) { __NOP(); } /* IWDG resets if already started. */
}

static void diagnostics(void *argument)
{
    (void)argument;
    static TaskStatus_t tasks[MAX_DIAGNOSTIC_TASKS];
    uint64_t previousTotal = 0u, previousIdle = 0u;
    LOG_INFO(LOG_MODULE_SYSTEM, "PERF boot cpu=%lu hclk=%lu pclk=%lu cache=%lu/%lu dma_nc=%u",
             (unsigned long)HAL_RCC_GetSysClockFreq(), (unsigned long)HAL_RCC_GetHCLKFreq(),
             (unsigned long)HAL_RCC_GetPCLK1Freq(),
             (unsigned long)((SCB->CCR & SCB_CCR_IC_Msk) != 0u),
             (unsigned long)((SCB->CCR & SCB_CCR_DC_Msk) != 0u), PLATFORM_DMA_BYTES);
    for (;;) {
        osDelay(pdMS_TO_TICKS(10000u));
        SafetySupervisor_ReportTiming();
        uint64_t total = 0u, idle = 0u;
        const UBaseType_t count = uxTaskGetSystemState(tasks, MAX_DIAGNOSTIC_TASKS, &total);
        if (!count) {
            LOG_WARN(LOG_MODULE_SYSTEM, "PERF task snapshot capacity exceeded");
            continue;
        }
        for (UBaseType_t i=0u; i<count; ++i)
            if (tasks[i].xHandle == xTaskGetIdleTaskHandle()) idle = tasks[i].ulRunTimeCounter;
        const uint64_t elapsed = total-previousTotal;
        const uint64_t idleElapsed = idle-previousIdle;
        const unsigned busy = elapsed ? (unsigned)(1000u - (idleElapsed*1000u/elapsed > 1000u
                              ? 1000u : idleElapsed*1000u/elapsed)) : 0u;
        previousTotal=total; previousIdle=idle;
        LOG_INFO(LOG_MODULE_SYSTEM,"PERF cpu_permille=%u heap=%lu min=%lu tasks=%lu rx_drop=%lu log_drop=%lu",
                 busy,(unsigned long)xPortGetFreeHeapSize(),
                 (unsigned long)xPortGetMinimumEverFreeHeapSize(),(unsigned long)count,
                 (unsigned long)UART_GetRxDropCount(), (unsigned long)LogManager_DroppedCount());
        /* Watermarks are measured minimum free bytes, not current SP distance. */
        for (UBaseType_t i=0u; i<count; ++i) {
            LOG_INFO(LOG_MODULE_SYSTEM,"STACK %s free_min=%lu priority=%lu run_ms=%lu",
                     tasks[i].pcTaskName,(unsigned long)(tasks[i].usStackHighWaterMark*sizeof(StackType_t)),
                     (unsigned long)tasks[i].uxCurrentPriority,
                     (unsigned long)(tasks[i].ulRunTimeCounter/1000u));
            osDelay(pdMS_TO_TICKS(2u));
        }
    }
}

void RuntimeStats_TaskInit(void)
{
    const osThreadAttr_t attributes = {
        .name="Diagnostics", .stack_size=3072u, .priority=osPriorityLow
    };
    if (!osThreadNew(diagnostics, NULL, &attributes)) RuntimeStats_Fatal(3u);
}
