#include "imu_task.h"

#include <stdio.h>

#include "cmsis_os.h"
#include "error_code.h"
#include "freertos_os2.h"
#include "log_manager.h"
#include "system_init.h"
#include "task.h"

#define IMU_TASK_PERIOD_MS 10u /* 100 Hz */
#define IMU_LOG_PERIOD_MS 100u /* 10 Hz */
#define IMU_TIMEOUT_MS 2000u

static osThreadId_t imuTaskHandle;
static ImuService* imuService;
static ImuService s_imuFallback;

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
