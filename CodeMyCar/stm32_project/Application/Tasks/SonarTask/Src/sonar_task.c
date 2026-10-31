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

static osThreadId_t s_taskHandle = NULL;

static SafetyChannel s_frontSafety;
static SafetyChannel s_rearSafety;

static uint8_t s_txSeq = 0u;
static void    send_sensor_frame(void);
static void    log_status(void);

void SonarTask_Init(void)
{
    safety_channel_init(&s_frontSafety, ULTRASONIC_FRONT_ENABLED,
                        FRONT_OFFSET_CM, SAFETY_STOP_CM, SAFETY_SLOW_CM);
    safety_channel_init(&s_rearSafety, ULTRASONIC_REAR_ENABLED,
                        REAR_OFFSET_CM, SAFETY_STOP_CM, SAFETY_SLOW_CM);

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

        /* 先取上一次结果（测量在 30 ms 内必然结束，50 ms 后肯定就绪） */
        if (ULTRASONIC_FRONT_ENABLED && Ultrasonic_Poll(front, now, &distance))
        {
            safety_channel_update(&s_frontSafety, distance);
        }
        if (ULTRASONIC_REAR_ENABLED && Ultrasonic_Poll(rear, now, &distance))
        {
            safety_channel_update(&s_rearSafety, distance);
        }

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

void Sonar_GetSafety(const SafetyChannel** front, const SafetyChannel** rear)
{
    if (front != NULL)
    {
        *front = &s_frontSafety;
    }
    if (rear != NULL)
    {
        *rear = &s_rearSafety;
    }
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
