#include "imu_service.h"

#include <math.h>
#include <string.h>

#include "error_code.h"
#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"

#define IMU_RAD2DEG 57.29578f
#define IMU_CALIB_SAMPLES 100u /* 1 s 静止样本 */
#define IMU_CALIB_MAX_DPS 5.0f /* 超过视为运动，丢弃样本 */
#define IMU_REPORT_INTERVAL_US 10000u /* 100 Hz */

extern sh2_Hal_t sh2_hal_stm32;

static float wrap180(float deg)
{
    while (deg > 180.0f)
    {
        deg -= 360.0f;
    }
    while (deg < -180.0f)
    {
        deg += 360.0f;
    }
    return deg;
}

static void imu_event_cb(void* cookie, sh2_AsyncEvent_t* event)
{
    (void)cookie;
    (void)event;
}

static void imu_sensor_cb(void* cookie, sh2_SensorEvent_t* event)
{
    ImuService* s = (ImuService*)cookie;
    if (s == NULL)
    {
        return;
    }

    sh2_SensorValue_t value;
    if (sh2_decodeSensorEvent(&value, event) != SH2_OK)
    {
        return;
    }

    switch (value.sensorId)
    {
        case SH2_GAME_ROTATION_VECTOR:
            s->quatX = value.un.gameRotationVector.i;
            s->quatY = value.un.gameRotationVector.j;
            s->quatZ = value.un.gameRotationVector.k;
            s->quatW = value.un.gameRotationVector.real;
            s->status = value.status;
            s->grvCount++;
            s->lastUpdateMs = HAL_GetTick();
            break;

        case SH2_GYROSCOPE_CALIBRATED:
            s->gyroX = value.un.gyroscope.x;
            s->gyroY = value.un.gyroscope.y;
            s->gyroZ = value.un.gyroscope.z;
            s->gyroCount++;
            break;

        case SH2_ACCELEROMETER:
            s->accelX = value.un.accelerometer.x;
            s->accelY = value.un.accelerometer.y;
            s->accelZ = value.un.accelerometer.z;
            s->accelCount++;
            break;

        default:
            break;
    }
}

int ImuService_Init(ImuService* s, I2C_HandleTypeDef* i2c)
{
    (void)i2c; /* 传输层固定使用 hi2c1，见 sh2_hal_stm32.c */
    if (s == NULL)
    {
        return ERR_INVALID_PARAM;
    }
    memset(s, 0, sizeof(*s));

    if (sh2_open(&sh2_hal_stm32, imu_event_cb, s) != SH2_OK)
    {
        return ERR_HAL_SERVO_FAILURE;
    }
    (void)sh2_setSensorCallback(imu_sensor_cb, s);

    sh2_SensorConfig_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reportInterval_us = IMU_REPORT_INTERVAL_US;
    (void)sh2_setSensorConfig(SH2_GAME_ROTATION_VECTOR, &cfg);
    (void)sh2_setSensorConfig(SH2_GYROSCOPE_CALIBRATED, &cfg);

    sh2_SensorConfig_t accelCfg;
    memset(&accelCfg, 0, sizeof(accelCfg));
    accelCfg.reportInterval_us = 20000u; /* 50 Hz，上报按 10 Hz */
    (void)sh2_setSensorConfig(SH2_ACCELEROMETER, &accelCfg);

    s->inited = true;
    s->calibrating = true;
    s->calibCount = 0u;
    s->calibSumDps = 0.0;
    return ERR_SUCCESS;
}

void ImuService_Close(ImuService* s)
{
    if (s == NULL)
    {
        return;
    }
    sh2_close();
    s->inited = false;
    s->yawValid = false;
    s->lastRawValid = false;
    s->lastUpdateMs = 0u;
}

void ImuService_Update(ImuService* s)
{
    if (s == NULL || !s->inited)
    {
        return;
    }
    s->seq++; /* 进入写入区（奇数） */

    /* 每次多服务几次：传感器 100 Hz 上报，单次调用可能积压 */
    for (uint8_t i = 0u; i < 8u; i++)
    {
        sh2_service();
    }

    const float gyroZDps = s->gyroZ * IMU_RAD2DEG;
    const bool newGyroSample = (s->gyroCount != s->calibLastGyroCount);
    s->calibLastGyroCount = s->gyroCount;

    if (s->calibrating && newGyroSample)
    {
        /* 只在接近静止时采样：车在转动时把样本丢弃并重新计数 */
        if (fabsf(gyroZDps) > IMU_CALIB_MAX_DPS)
        {
            s->calibCount = 0u;
            s->calibSumDps = 0.0;
        }
        else
        {
            s->calibSumDps += (double)gyroZDps;
            s->calibCount++;
        }
        if (s->calibCount >= IMU_CALIB_SAMPLES)
        {
            s->biasDps = (float)(s->calibSumDps / (double)s->calibCount);
            s->calibrating = false;
            s->biasValid = true;
        }
    }
    else if (s->biasValid && newGyroSample && fabsf(gyroZDps - s->biasDps) < 0.5f)
    {
        /* 长时间静止时缓慢跟踪零偏漂移 */
        s->biasDps += 0.0005f * (gyroZDps - s->biasDps);
    }
    s->yawRateDps = gyroZDps - s->biasDps;

    const float qw = s->quatW;
    const float qx = s->quatX;
    const float qy = s->quatY;
    const float qz = s->quatZ;
    const float norm2 = qw * qw + qx * qx + qy * qy + qz * qz;
    if (!isfinite(norm2) || norm2 < 0.5f || norm2 > 1.5f)
    {
        s->seq++; /* 退出写入区 */
        return; /* 尚无有效四元数或数值异常 */
    }

    const float raw = atan2f(2.0f * (qw * qz + qx * qy),
                             1.0f - 2.0f * (qy * qy + qz * qz)) * IMU_RAD2DEG;
    if (!isfinite(raw))
    {
        s->seq++;
        return;
    }

    if (!s->lastRawValid)
    {
        s->yawDeg = raw;
        s->lastRawYawDeg = raw;
        s->lastRawValid = true;
    }
    else
    {
        s->yawDeg += wrap180(raw - s->lastRawYawDeg);
        s->lastRawYawDeg = raw;
    }
    s->yawValid = true;
    s->seq++; /* 退出写入区（偶数） */
}

void ImuService_ResetYawReference(ImuService* s)
{
    if (s != NULL)
    {
        s->yawDeg = 0.0f;
    }
}

bool ImuService_GetSnapshot(ImuService* s, ImuSnapshot* out)
{
    if (s == NULL || out == NULL)
    {
        return false;
    }
    for (uint8_t attempt = 0u; attempt < 3u; attempt++)
    {
        const uint32_t s0 = s->seq;
        if ((s0 & 1u) != 0u)
        {
            continue; /* 正在写入 */
        }
        ImuSnapshot snap;
        snap.valid = s->yawValid;
        snap.yawDeg = s->yawDeg;
        snap.yawRateDps = s->yawRateDps;
        snap.status = s->status;
        snap.lastUpdateMs = s->lastUpdateMs;
        if (s->seq == s0)
        {
            *out = snap;
            return true;
        }
    }
    return false;
}
