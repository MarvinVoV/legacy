#include "safety_supervisor.h"

#include "cmsis_os2.h"
#include "error_code.h"
#include "log_manager.h"
#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"
#include "task_timing.h"

#define SUPERVISOR_PERIOD_MS 50u
#define TASK_STALE_TIMEOUT_MS 250u
#define STARTUP_GRACE_MS 1000u

static TaskHealthMonitor s_health;
static MotionContext* s_motionContext;
static osThreadId_t s_taskHandle;
static bool s_ready;
static TaskTimingStats s_timing[TASK_HEALTH_COUNT];

static const char* timing_name(const TaskHealthId id)
{
    static const char* names[TASK_HEALTH_COUNT] = {"motion", "motor_l", "motor_r", "ctrl", "servo"};
    return id < TASK_HEALTH_COUNT ? names[id] : "unknown";
}

static void watchdog_start(void)
{
#ifdef DBGMCU_APB4FZ1_DBG_IWDG1
    DBGMCU->APB4FZ1 |= DBGMCU_APB4FZ1_DBG_IWDG1;
#endif
    /* LSI ~= 32kHz, /64, reload 1999 => about four seconds. */
    IWDG1->KR = 0x5555u;
    IWDG1->PR = 4u;
    IWDG1->RLR = 1999u;
    IWDG1->KR = 0xCCCCu;
    IWDG1->KR = 0xAAAAu;
}

static void watchdog_refresh(void)
{
    IWDG1->KR = 0xAAAAu;
}

static void supervisor_task(void* argument)
{
    (void)argument;
    bool faultHandled = false;
    while (1)
    {
        const uint32_t stale = task_health_check(&s_health, HAL_GetTick(),
                                                  TASK_STALE_TIMEOUT_MS);
        if (stale == 0u)
        {
            watchdog_refresh();
        }
        else
        {
            if (!faultHandled)
            {
                faultHandled = true;
                LOG_ERROR(LOG_MODULE_SYSTEM, "task health stale mask=0x%02X",
                          (unsigned)stale);
                MotionService_LatchFault(s_motionContext, MOTION_ERR_TASK_STALE);
            }
            /* Do not refresh: hardware reset is the recovery path. */
        }
        osDelay(SUPERVISOR_PERIOD_MS);
    }
}

int SafetySupervisor_Init(MotionContext* motionContext)
{
    if (motionContext == NULL)
    {
        return ERR_INVALID_PARAM;
    }
    const uint32_t required = (1u << TASK_HEALTH_MOTION) |
                              (1u << TASK_HEALTH_MOTOR_LEFT) |
                              (1u << TASK_HEALTH_MOTOR_RIGHT) |
                              (1u << TASK_HEALTH_CTRL) |
                              (1u << TASK_HEALTH_SERVO);
    s_motionContext = motionContext;
    task_health_init(&s_health, required, HAL_GetTick(), STARTUP_GRACE_MS);
    const uint32_t now = HAL_GetTick();
    task_timing_init(&s_timing[TASK_HEALTH_MOTION], 5u, 2u, now);
    task_timing_init(&s_timing[TASK_HEALTH_MOTOR_LEFT], 5u, 2u, now);
    task_timing_init(&s_timing[TASK_HEALTH_MOTOR_RIGHT], 5u, 2u, now);
    task_timing_init(&s_timing[TASK_HEALTH_CTRL], 50u, 20u, now);
    task_timing_init(&s_timing[TASK_HEALTH_SERVO], 20u, 5u, now);
    s_ready = true;
    watchdog_start();

    const osThreadAttr_t attributes = {
        .name = "SafetySupervisor",
        .stack_size = 512u * 4u,
        .priority = (osPriority_t)osPriorityHigh,
    };
    s_taskHandle = osThreadNew(supervisor_task, NULL, &attributes);
    return s_taskHandle != NULL ? ERR_SUCCESS : ERR_HW_INIT_FAIL;
}

void SafetySupervisor_Heartbeat(TaskHealthId id)
{
    if (s_ready && id < TASK_HEALTH_COUNT)
    {
        const uint32_t now = HAL_GetTick();
        task_health_beat(&s_health, id, now);
        TaskTimingStats* timing = &s_timing[id];
        task_timing_record(timing, now);
        if (task_timing_report_due(timing, now, 10000u))
        {
            LOG_INFO(LOG_MODULE_SYSTEM,
                     "RTOS %s n=%lu min=%lums max=%lums miss=%lu budget=%lums",
                     timing_name(id), (unsigned long)timing->samples,
                     (unsigned long)timing->minIntervalMs,
                     (unsigned long)timing->maxIntervalMs,
                     (unsigned long)timing->deadlineMisses,
                     (unsigned long)(timing->expectedPeriodMs + timing->toleranceMs));
            task_timing_reset_window(timing, now);
        }
    }
}
