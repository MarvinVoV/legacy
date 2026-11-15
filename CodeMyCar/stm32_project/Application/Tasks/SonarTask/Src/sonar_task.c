/*
 * sonar_task.c
 *
 * 超声波测距任务：
 *  - 50 ms 节拍，前后传感器错开一拍触发（各 10 Hz）
 *  - 取结果 → 更新安全通道（中值滤波 / 故障判定 / 比例计算）
 *  - 每 200 ms 上报一帧（前向距离 + 后向距离）
 *  - 每 1 s 打一条状态日志
 */

#include "sonar_task.h"

#include <stddef.h>

#include "cmsis_os2.h"
#include "log_manager.h"
#include "pkt_protocol.h"
#include "sensor_protocol.h"
#include "sonar_hw.h"
#include "uart_handle.h"
#include "ultrasonic_config.h"
#include "buzzer.h"
#include "control_critical.h"
#include "sonar_config.h"
#include "system_init.h"
#include "queue.h"

static osThreadId_t s_taskHandle = NULL;

#define HAS_SONAR (ULTRASONIC_FRONT_ENABLED || ULTRASONIC_REAR_ENABLED)
static SonarConfig config = {.enabled = HAS_SONAR};
static StaticQueue_t request_queue_storage;
static SonarRequest request_slots[4];
static QueueHandle_t request_queue;

static SafetyChannel s_frontSafety;
static SafetyChannel s_rearSafety;
static SafetyChannel s_publishedFront;
static SafetyChannel s_publishedRear;

static void publish_safety(void)
{
    const uint32_t key = control_critical_enter();
    s_publishedFront = s_frontSafety;
    s_publishedRear = s_rearSafety;
    /* Until the first new sample outside the startup path, preserve hard braking. */
    if (config.enabled && config.generation) {
        if (s_publishedFront.enabled && !s_publishedFront.everValid) s_publishedFront.fault = 1;
        if (s_publishedRear.enabled && !s_publishedRear.everValid) s_publishedRear.fault = 1;
    }
    control_critical_exit(key);
}

static uint8_t s_txSeq = 0u;
static void    send_sensor_frame(void);
static void    log_status(void);

static void reset_windows(void)
{
    safety_channel_init(&s_frontSafety, ULTRASONIC_FRONT_ENABLED && config.enabled,
                        FRONT_OFFSET_CM, SAFETY_STOP_CM, SAFETY_SLOW_CM);
    safety_channel_init(&s_rearSafety, ULTRASONIC_REAR_ENABLED && config.enabled,
                        REAR_OFFSET_CM, SAFETY_STOP_CM, SAFETY_SLOW_CM);
    s_frontSafety.startedMs = s_rearSafety.startedMs = HAL_GetTick();
    publish_safety();

}

void SonarTask_Init(void)
{
    reset_windows();
    request_queue = xQueueCreateStatic(4, sizeof(SonarRequest), (uint8_t *)request_slots, &request_queue_storage);
    const osThreadAttr_t attrs = {
        .name       = "SonarTask",
        .stack_size = 512 * 4,
        .priority   = (osPriority_t)osPriorityBelowNormal,
    };
    s_taskHandle = osThreadNew(SonarTask_doTask, NULL, &attrs);
    if (s_taskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SENSOR, "Failed to create SonarTask");
    }
}

void SonarTask_Submit(const uint8_t *payload, uint16_t len)
{
    SonarRequest request;
    if (request_queue && sonar_decode_request(payload, len, &request) &&
        xQueueSend(request_queue, &request, 0) != pdPASS) {
        LOG_WARN(LOG_MODULE_SENSOR, "sonar config queue full");
    }
}

static void apply_request(const SonarRequest *request, UltrasonicSensor *front, UltrasonicSensor *rear)
{
    MotionContext *motion = System_GetContext()->motionContext;
    bool locked = false;
    bool parked = false;
    if (request->op == SONAR_SET && !request->enable && config.enabled && motion) {
        locked = osMutexAcquire(motion->targetMutex, 0) == osOK;
        if (locked) parked = MotionService_IsParkedLocked(motion);
    }
    if (sonar_apply(&config, request, HAL_GetTick(), HAS_SONAR, parked)) {
        const uint32_t key = control_critical_enter();
        front->phase = rear->phase = US_PHASE_IDLE;
        front->result = rear->result = 0;
        control_critical_exit(key);
        Buzzer_Set(false);
        reset_windows();
    }
    if (locked) osMutexRelease(motion->targetMutex);
}

static bool channel_ready(const SafetyChannel *ch, uint32_t now)
{
    return !ch->enabled || (ch->everValid && !ch->fault && now-ch->lastValidMs <= SAFETY_DATA_TIMEOUT_MS);
}

static void send_config_status(uint32_t now)
{
    const bool ready = config.enabled && channel_ready(&s_frontSafety, now) && channel_ready(&s_rearSafety, now);
    uint8_t payload[SONAR_STATUS_SIZE] = {1u, (HAS_SONAR ? SONAR_SUPPORTED : 0u) |
        (config.enabled ? SONAR_ENABLED : 0u) | (ready ? SONAR_READY : 0u), config.result, 0u};
    sonar_write32(payload+4, config.epoch);
    sonar_write32(payload+8, config.generation);
    sonar_write32(payload+12, config.request_id);
    sonar_write32(payload+16, now);
    UART_Send_Frame(SONAR_STATUS_TYPE, 0, 0, payload, sizeof(payload));
}

void SonarTask_doTask(void* arg)
{
    (void)arg;

    UltrasonicSensor* front = SonarHw_Front();
    UltrasonicSensor* rear  = SonarHw_Rear();

    const TickType_t period      = pdMS_TO_TICKS(US_TASK_PERIOD_MS);
    TickType_t       lastWake    = xTaskGetTickCount();
    uint32_t         tick        = 0u;

    while (1)
    {
        const uint32_t now = HAL_GetTick();
        int16_t        distance;
        SonarRequest request;
        const bool received = request_queue && xQueueReceive(request_queue, &request, 0) == pdPASS;
        if (received) apply_request(&request, front, rear);
        if (received || tick % US_LOG_DIVIDER == 0) send_config_status(now);

        /* 运行期关闭：不触发、不上报、安全通道保持放行 */
        if (!config.enabled)
        {
            tick++;
            vTaskDelayUntil(&lastWake, period);
            continue;
        }

        /* 先取上一次结果（测量在 30 ms 内必然结束，50 ms 后肯定就绪） */
        if (ULTRASONIC_FRONT_ENABLED && Ultrasonic_Poll(front, now, &distance))
        {
            safety_channel_update(&s_frontSafety, distance, HAL_GetTick());
        }
        if (ULTRASONIC_REAR_ENABLED && Ultrasonic_Poll(rear, now, &distance))
        {
            safety_channel_update(&s_rearSafety, distance, HAL_GetTick());
        }

        publish_safety();
        /* 前后错开一拍触发 */
        if ((tick & 1u) == 0u)
        {
            if (ULTRASONIC_FRONT_ENABLED)
            {
                Ultrasonic_Start(front, now);
            }
        }
        else
        {
            if (ULTRASONIC_REAR_ENABLED)
            {
                Ultrasonic_Start(rear, now);
            }
        }

        if ((tick % US_REPORT_DIVIDER) == 0u)
        {
            send_sensor_frame();
        }
        if ((tick % US_LOG_DIVIDER) == 0u)
        {
            log_status();
        }

        /* 蜂鸣器：按运动方向选前/后通道，8~40cm 内越近节奏越快。
         * 只有存在运动指令时才响，停车（E-STOP / 速度为 0）一律静音。 */
        {
            const SafetyChannel* beep_ch =
                safety_is_reversing() ? &s_rearSafety : &s_frontSafety;
            const int16_t beep_dist = safety_is_motion_active()
                                          ? beep_ch->distanceCm
                                          : (int16_t)-1;
            Buzzer_Update(now, beep_dist);
        }

        tick++;
        vTaskDelayUntil(&lastWake, period);
    }
}

void Sonar_GetSafetySnapshot(SafetyChannel* front, SafetyChannel* rear)
{
    const uint32_t key = control_critical_enter();
    if (front != NULL) { *front = s_publishedFront; }
    if (rear != NULL) { *rear = s_publishedRear; }
    control_critical_exit(key);
}

/*------------------------------------------------------------------------------
 * 上报 / 日志
 *---------------------------------------------------------------------------*/

/**
 * 传感器帧 payload（9 字节，小端）：
 *   [timestamp u32][type=3][front_cm u16][rear_cm u16]
 * 无效距离填 0xFFFF。
 */
static void send_sensor_frame(void)
{
    uint8_t payload[9];
    const uint32_t ts = HAL_GetTick();

    payload[0] = (uint8_t)(ts & 0xFFu);
    payload[1] = (uint8_t)((ts >> 8) & 0xFFu);
    payload[2] = (uint8_t)((ts >> 16) & 0xFFu);
    payload[3] = (uint8_t)((ts >> 24) & 0xFFu);
    payload[4] = (uint8_t)SENSOR_ULTRASONIC;

    const uint16_t front = (s_frontSafety.distanceCm > 0)
                               ? (uint16_t)s_frontSafety.distanceCm
                               : 0xFFFFu;
    const uint16_t rear = (s_rearSafety.distanceCm > 0)
                              ? (uint16_t)s_rearSafety.distanceCm
                              : 0xFFFFu;

    payload[5] = (uint8_t)(front & 0xFFu);
    payload[6] = (uint8_t)(front >> 8);
    payload[7] = (uint8_t)(rear & 0xFFu);
    payload[8] = (uint8_t)(rear >> 8);

    UART_Send_Frame(PROTOCOL_TYPE_SENSOR, s_txSeq++, 0u, payload,
                    (uint16_t)sizeof(payload));
}

static const char* channel_state(const SafetyChannel* ch)
{
    if (!ch->enabled)
    {
        return "off";
    }
    return ch->fault ? "fault" : "ok";
}

static void log_status(void)
{
    LOG_INFO(LOG_MODULE_SENSOR,
             "US F=%dcm R=%dcm sF=%d%% sR=%d%% st=%s/%s",
             (int)s_frontSafety.distanceCm,
             (int)s_rearSafety.distanceCm,
             (int)(s_frontSafety.scale * 100.0f),
             (int)(s_rearSafety.scale * 100.0f),
             channel_state(&s_frontSafety),
             channel_state(&s_rearSafety));
}
