#include "imu_task.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "cmsis_os.h"
#include "error_code.h"
#include "freertos_os2.h"
#include "log_manager.h"
#include "system_init.h"
#include "uart_handle.h"
#include "pkt_protocol.h"
#include "sensor_protocol.h"
#include "task.h"

#define IMU_TASK_PERIOD_MS 10u /* 100 Hz */
#define IMU_LOG_PERIOD_MS 100u /* 10 Hz */
#define IMU_TIMEOUT_MS 2000u

static uint8_t s_sensorSeq;
static osThreadId_t imuTaskHandle;
static ImuService* imuService;
static ImuService s_imuFallback;

/* 10 Hz 上报航向帧（payload 10 字节）：
   ts(u32) + type(u8) + yaw(i16,0.1°) + yaw_rate(i16,0.01°/s) + status(u8) */
static void send_heading_frame(const ImuService* s)
{
    if (!s->yawValid)
    {
        return;
    }
    uint8_t payload[10];
    const uint32_t ts = HAL_GetTick();
    payload[0] = (uint8_t)(ts & 0xFFu);
    payload[1] = (uint8_t)((ts >> 8) & 0xFFu);
    payload[2] = (uint8_t)((ts >> 16) & 0xFFu);
    payload[3] = (uint8_t)((ts >> 24) & 0xFFu);
    payload[4] = (uint8_t)SENSOR_HEADING;

    int16_t yaw_ddeg = (int16_t)lroundf(s->yawDeg * 10.0f);
    int16_t rate_cdps = (int16_t)lroundf(s->yawRateDps * 100.0f);

    payload[5] = (uint8_t)((uint16_t)yaw_ddeg & 0xFFu);
    payload[6] = (uint8_t)((uint16_t)yaw_ddeg >> 8);
    payload[7] = (uint8_t)((uint16_t)rate_cdps & 0xFFu);
    payload[8] = (uint8_t)((uint16_t)rate_cdps >> 8);
    payload[9] = s->status;

    (void)UART_Send_Frame(PROTOCOL_TYPE_SENSOR, s_sensorSeq++, 0u, payload,
                          (uint16_t)sizeof(payload));
}

/* 10 Hz 上报 6 轴数据（payload 29 字节）：
   ts(u32) + type(u8) + accel[3](float) + gyro[3](float) */
static void send_imu_frame(const ImuService* s)
{
    if (!s->yawValid)
    {
        return;
    }
    uint8_t payload[29];
    const uint32_t ts = HAL_GetTick();
    payload[0] = (uint8_t)(ts & 0xFFu);
    payload[1] = (uint8_t)((ts >> 8) & 0xFFu);
    payload[2] = (uint8_t)((ts >> 16) & 0xFFu);
    payload[3] = (uint8_t)((ts >> 24) & 0xFFu);
    payload[4] = (uint8_t)SENSOR_IMU;

    const float vals[6] = {s->accelX, s->accelY, s->accelZ,
                           s->gyroX, s->gyroY, s->gyroZ};
    memcpy(&payload[5], vals, sizeof(vals));

    (void)UART_Send_Frame(PROTOCOL_TYPE_SENSOR, s_sensorSeq++, 0u, payload,
                          (uint16_t)sizeof(payload));
}

static void ImuTask_doTask(void* pvParameters)
{
    ImuService* svc = (ImuService*)pvParameters;
    uint32_t lastWake = osKernelGetTickCount();
    uint32_t lastLog = lastWake;
    bool needInit = true;

    while (1)
    {
        /* 初始化放在任务里：默认任务栈只有 512 B，SH-2 协议栈缓冲放不下 */
        if (needInit)
        {
            needInit = false;
            /* 地址探测：区分 NOACK / BUSY / OK */
            const HAL_StatusTypeDef rdyA = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(0x4Au << 1), 3u, 50u);
            const HAL_StatusTypeDef rdyB = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(0x4Bu << 1), 3u, 50u);
            LOG_INFO(LOG_MODULE_SENSOR, "IMU probe: 0x4A=%d 0x4B=%d (0=OK 1=ERR 2=BUSY 3=TIMEOUT)",
                     (int)rdyA, (int)rdyB);

            const int rc = ImuService_Init(svc, &hi2c1);
            if (rc != ERR_SUCCESS)
            {
                LOG_WARN(LOG_MODULE_SENSOR, "IMU init failed: 0x%04X", rc);
            }
            else
            {
                LOG_INFO(LOG_MODULE_SENSOR, "IMU ready (SH-2)");
            }
        }

        ImuService_Update(svc);

        /* 超过 2 s 没有姿态数据：重开 SH-2 会话（含硬复位） */
        if (svc->inited && svc->lastUpdateMs != 0u &&
            HAL_GetTick() - svc->lastUpdateMs > IMU_TIMEOUT_MS)
        {
            LOG_WARN(LOG_MODULE_SENSOR, "IMU timeout -> reopen SH-2");
            ImuService_Close(svc);
            needInit = true;
        }

        const uint32_t now = osKernelGetTickCount();
        if (now - lastLog >= IMU_LOG_PERIOD_MS)
        {
            lastLog = now;
            if (svc->yawValid)
            {
                LOG_INFO(LOG_MODULE_SENSOR,
                         "IMU yaw=%.2f deg rate=%.2f dps bias=%.3f st=%u grv=%lu gyro=%lu",
                         svc->yawDeg, svc->yawRateDps, svc->biasDps,
                         (unsigned)svc->status,
                         (unsigned long)svc->grvCount,
                         (unsigned long)svc->gyroCount);
            }
            else
            {
                extern volatile uint32_t sh2_diag_reads, sh2_diag_packets;
                extern volatile uint8_t sh2_diag_last_hdr[4], sh2_diag_last_pl[4];
                LOG_WARN(LOG_MODULE_SENSOR,
                         "IMU no data reads=%lu pkts=%lu hdr=%02X %02X %02X %02X pl=%02X %02X %02X %02X",
                         (unsigned long)sh2_diag_reads,
                         (unsigned long)sh2_diag_packets,
                         sh2_diag_last_hdr[0], sh2_diag_last_hdr[1],
                         sh2_diag_last_hdr[2], sh2_diag_last_hdr[3],
                         sh2_diag_last_pl[0], sh2_diag_last_pl[1],
                         sh2_diag_last_pl[2], sh2_diag_last_pl[3]);
            }
            send_heading_frame(svc); /* 10 Hz 上报航向 */
            send_imu_frame(svc);     /* 10 Hz 上报六轴原始数据 */
        }

        osDelayUntil(lastWake + pdMS_TO_TICKS(IMU_TASK_PERIOD_MS));
        lastWake = osKernelGetTickCount();
    }
}

void ImuTask_Init(ImuService* service)
{
    imuService = (service != NULL) ? service : &s_imuFallback;

    const osThreadAttr_t attr = {
        .name = "ImuTask",
        .stack_size = 1024 * 4, /* SH-2 协议栈需要较大栈 */
        .priority = (osPriority_t)osPriorityNormal,
    };
    imuTaskHandle = osThreadNew(ImuTask_doTask, imuService, &attr);
    if (imuTaskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SENSOR, "Failed to create ImuTask");
    }
}

ImuService* ImuTask_GetService(void)
{
    return imuService;
}
