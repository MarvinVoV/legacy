#include "light_task.h"
#include "led_strip.h"
#include "lighting_service.h"
#include "lighting_runtime.h"
#include "system_init.h"
#include "power_task.h"
#include "steering_lut.h"
#include "uart_handle.h"
#include <math.h>
#include "cmsis_os2.h"
#include "freertos_os2.h"
#include "stm32h7xx_hal.h"
#include "log_manager.h"

/* This adapter only tries the transaction mutex with timeout=0 and copies a
 * few scalars; it never waits on motion/servo locks or calls actuator APIs. */
static void read_local(LightLocal *local, uint32_t now)
{
    SystemContext *system=System_GetContext();
    if (system && system->motionContext) {
        MotionContext *ctx=system->motionContext;
        if (osMutexAcquire(ctx->targetMutex,0u)==osOK) {
            local->sampledMs=ctx->state.timestamp;
            local->motionValid=ctx->motorStateValid;
            local->fault=ctx->faultLatched;
            local->braking=ctx->safetyBraked || (ctx->goalSettling && !ctx->goal.finished);
            local->stopRequested=ctx->motionTarget.mode==MOTION_EMERGENCY_STOP || ctx->stopPending;
            const float speed=ctx->state.velocity.linear*1000.0f;
            local->speedMmps=isfinite(speed) ? (int16_t)fmaxf(-2000.0f,fminf(2000.0f,speed)) : 0;
            osMutexRelease(ctx->targetMutex);
        }
    }
    /* Submitted pulse is observable without touching a servo mutex. It is not
     * feedback from a wheel-angle sensor. Hysteresis ignores small hold corrections. */
    const int pulse=(int)__HAL_TIM_GET_COMPARE(&htim2,TIM_CHANNEL_1);
    const int delta=pulse-(int)STEERING_SERVO_STRAIGHT_US;
    if (delta>60) local->steering=-1;
    else if (delta<-60) local->steering=1;
    else if (delta>-30 && delta<30) local->steering=0;
    BatteryReading battery;
    if (PowerTask_GetReading(&battery)) {
        local->batteryMs=battery.timestamp;
        local->batteryValid=(battery.flags&(BATTERY_VALID|BATTERY_SOC_VALID))==
                                           (BATTERY_VALID|BATTERY_SOC_VALID);
        local->batteryPercent=battery.percent;
    }
    (void)now;
}

static void light_run(void *arg)
{
    (void)arg;
    if (!LedStrip_Init()) {
        LOG_ERROR(LOG_MODULE_SYSTEM,"LED init failed; vehicle control remains independent");
        osThreadExit();return;
    }
    LightRenderer renderer={0};
    LightLocal local={0};
    uint32_t lastReport=0u,lastLog=0u;
    uint8_t lastReportedMode=255u;
    for (;;) {
        const uint32_t wake=osKernelGetTickCount(),now=HAL_GetTick();
        read_local(&local,now);
        LightHints hints;LightView view;
        LightingRuntime_GetHints(&hints);
        LedRgb pixels[WS2812_LED_COUNT];
        Lighting_Render(&renderer,&hints,&local,now,pixels,&view);
        (void)LedStrip_Show(pixels);
        if (now-lastReport>=1000u || (view.mode!=lastReportedMode && now-lastReport>=100u)) {
            lastReport=now;lastReportedMode=view.mode;
            uint8_t status[LIGHTING_STATUS_SIZE];
            Lighting_PackStatus(&hints,&view,pixels,now,status);
            (void)UART_Send_Frame(PROTOCOL_TYPE_VISUAL,0u,0u,status,sizeof(status));
        }
        if (now-lastLog>=5000u) {
            lastLog=now;
            LOG_INFO(LOG_MODULE_SYSTEM,"LED frames=%lu errors=%lu mode=%u hint=%u/%u",
                     (unsigned long)LedStrip_CompletedFrames(),(unsigned long)LedStrip_ErrorCount(),
                     view.mode,hints.accepted,hints.rejected);
        }
        osDelayUntil(wake+pdMS_TO_TICKS(40u));
    }
}

void LightTask_Init(void)
{
    const osThreadAttr_t attrs={.name="LightTask",.stack_size=2048,.priority=osPriorityLow};
    if (!osThreadNew(light_run,NULL,&attrs))
        LOG_ERROR(LOG_MODULE_SYSTEM,"Failed to create LightTask");
}
