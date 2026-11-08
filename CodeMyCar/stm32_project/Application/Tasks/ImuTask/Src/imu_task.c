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

    if (!isfinite(s->yawDeg) || !isfinite(s->yawRateDps))
    {
        return;
    }

    /* 连续角回绕到 [-180,180]，避免 int16 在约 9 圈后溢出 */
    const float yawWrapped = remainderf(s->yawDeg, 360.0f);

    long yaw_l = lroundf(yawWrapped * 10.0f);
    long rate_l = lroundf(fmaxf(-327.67f, fminf(327.67f, s->yawRateDps)) * 100.0f);
    if (yaw_l > 1800L)   { yaw_l = 1800L; }
    if (yaw_l < -1800L)  { yaw_l = -1800L; }
    if (rate_l > 32767L) { rate_l = 32767L; }
    if (rate_l < -32767L){ rate_l = -32767L; }
    const int16_t yaw_ddeg = (int16_t)yaw_l;
    const int16_t rate_cdps = (int16_t)rate_l;

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
    uint32_t lastTimingLog = lastWake;
    bool needInit = true;
    uint32_t nextInitTick = 0u;
    uint32_t initDoneHal = 0u;

    while (1)
    {
        /* 初始化放在任务里：默认任务栈只有 512 B，SH-2 协议栈缓冲放不下 */
        if (needInit && (int32_t)(osKernelGetTickCount() - nextInitTick) >= 0)
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
                LOG_WARN(LOG_MODULE_SENSOR, "IMU init failed: 0x%04X (retry in 2s)", rc);
                needInit = true;
                nextInitTick = osKernelGetTickCount() + 2000u;
            }
            else
            {
                initDoneHal = HAL_GetTick();
                LOG_INFO(LOG_MODULE_SENSOR, "IMU ready (SH-2)");
            }
        }

        ImuService_Update(svc);

        /* 数据过期（2 s）或初始化后 1.5 s 仍无首帧 → 重开会话 */
        if (svc->inited)
        {
            const uint32_t nowHal = HAL_GetTick();
            const bool stale =
                (svc->grvCount != 0u && nowHal - svc->lastUpdateMs > IMU_TIMEOUT_MS) ||
                (svc->gyroCount != 0u && nowHal - svc->lastGyroMs > IMU_TIMEOUT_MS);
            const bool noFirst = (svc->grvCount == 0u || svc->gyroCount == 0u) &&
                                 (nowHal - initDoneHal > 1500u);
            if (stale || noFirst)
            {
                LOG_WARN(LOG_MODULE_SENSOR, "IMU %s -> reopen SH-2",
                         noFirst ? "no first frame" : "timeout");
                ImuService_Close(svc);
                needInit = true;
                nextInitTick = osKernelGetTickCount() + 500u;
            }
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
            if (now-lastTimingLog >= 1000u && svc->grvCount && svc->gyroCount)
            {
                lastTimingLog = now;
                LOG_INFO(LOG_MODULE_SENSOR,
                         "IMU_TIME grv_rx=%ld gyro_rx=%ld pair=%ld max=%ld/%ld us",
                         (long)svc->grvRxAgeUs, (long)svc->gyroRxAgeUs,
                         (long)(int32_t)(svc->grvSampleUs-svc->gyroSampleUs),
                         (long)svc->grvMaxRxAgeUs, (long)svc->gyroMaxRxAgeUs);
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
