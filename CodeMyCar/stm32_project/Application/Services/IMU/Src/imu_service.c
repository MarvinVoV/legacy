#include "imu_service.h"
#include <math.h>
#include <string.h>
#include "control_critical.h"
#include "error_code.h"
#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"

#define IMU_RAD2DEG 57.29578f
#define IMU_REPORT_INTERVAL_US 10000u
#define IMU_STALE_MS 200u
extern sh2_Hal_t sh2_hal_stm32;
static uint32_t s_referenceId;

static void publish(ImuService* s)
{
    const uint32_t now = HAL_GetTick();
    ImuSnapshot snap = {
        .valid = s->inited && s->yawValid && s->gyroValid &&
                 s->grvCount != 0u && s->gyroCount != 0u &&
                 now - s->lastUpdateMs <= IMU_STALE_MS &&
                 now - s->lastGyroMs <= IMU_STALE_MS &&
                 isfinite(s->yawDeg) && isfinite(s->yawRateDps),
        .yawDeg = s->yawDeg,
        .yawRateDps = s->yawRateDps,
        .status = s->status,
        .lastUpdateMs = s->lastUpdateMs,
        .lastGyroMs = s->lastGyroMs,
        .referenceId = s->referenceId,
    };
    const uint32_t key = control_critical_enter();
    s->published = snap;
    control_critical_exit(key);
}

static void imu_event_cb(void* cookie, sh2_AsyncEvent_t* event)
{
    ImuService* s = cookie;
    if (s != NULL && event->eventId == SH2_RESET)
    {
        s->referenceId = ++s_referenceId;
        s->lastRawValid = false;
        s->yawValid = false;
        s->gyroValid = false;
    }
}

static void imu_sensor_cb(void* cookie, sh2_SensorEvent_t* event)
{
    ImuService* s = cookie;
    sh2_SensorValue_t value;
    if (s == NULL || sh2_decodeSensorEvent(&value, event) != SH2_OK) { return; }
    switch (value.sensorId)
    {
        case SH2_GAME_ROTATION_VECTOR:
            s->quatX = value.un.gameRotationVector.i;
            s->quatY = value.un.gameRotationVector.j;
            s->quatZ = value.un.gameRotationVector.k;
            s->quatW = value.un.gameRotationVector.real;
            s->status = value.status;
            s->grvSampleUs = (uint32_t)value.timestamp;
            s->grvRxAgeUs = (int32_t)((uint32_t)(HAL_GetTick()*1000u)-s->grvSampleUs);
            if (s->grvRxAgeUs > s->grvMaxRxAgeUs) { s->grvMaxRxAgeUs = s->grvRxAgeUs; }
            s->grvCount++;
            break;
        case SH2_GYROSCOPE_CALIBRATED:
            s->gyroX = value.un.gyroscope.x;
            s->gyroY = value.un.gyroscope.y;
            s->gyroZ = value.un.gyroscope.z;
            s->gyroSampleUs = (uint32_t)value.timestamp;
            s->gyroRxAgeUs = (int32_t)((uint32_t)(HAL_GetTick()*1000u)-s->gyroSampleUs);
            if (s->gyroRxAgeUs > s->gyroMaxRxAgeUs) { s->gyroMaxRxAgeUs = s->gyroRxAgeUs; }
            s->gyroCount++;
            s->gyroValid = isfinite(s->gyroX) && isfinite(s->gyroY) && isfinite(s->gyroZ);
            if (s->gyroValid)
            {
                s->lastGyroMs = HAL_GetTick();
                /* This SH-2 report is already calibrated. Do not absorb real
                 * slow vehicle rotation into an application-side bias. */
                s->yawRateDps = s->gyroZ * IMU_RAD2DEG;
                s->biasValid = true;
            }
            else { s->yawRateDps = 0.0f; }
            break;
        case SH2_ACCELEROMETER:
            s->accelX = value.un.accelerometer.x;
            s->accelY = value.un.accelerometer.y;
            s->accelZ = value.un.accelerometer.z;
            s->accelCount++;
            break;
        default: break;
    }
}

int ImuService_Init(ImuService* s, I2C_HandleTypeDef* i2c)
{
    (void)i2c;
    if (s == NULL) { return ERR_INVALID_PARAM; }
    const uint32_t key = control_critical_enter();
    memset(s, 0, sizeof(*s));
    s->referenceId = ++s_referenceId;
    control_critical_exit(key);
    if (sh2_open(&sh2_hal_stm32, imu_event_cb, s) != SH2_OK)
    {
        sh2_close();
        return ERR_HW_INIT_FAIL;
    }
    sh2_SensorConfig_t cfg = {0};
    cfg.reportInterval_us = IMU_REPORT_INTERVAL_US;
    sh2_SensorConfig_t accelCfg = {0};
    accelCfg.reportInterval_us = 20000u;
    if (sh2_setSensorCallback(imu_sensor_cb, s) != SH2_OK ||
        sh2_setSensorConfig(SH2_GAME_ROTATION_VECTOR, &cfg) != SH2_OK ||
        sh2_setSensorConfig(SH2_GYROSCOPE_CALIBRATED, &cfg) != SH2_OK ||
        sh2_setSensorConfig(SH2_ACCELEROMETER, &accelCfg) != SH2_OK)
    {
        ImuService_Close(s);
        return ERR_HW_INIT_FAIL;
    }
    s->inited = true;
    publish(s);
    return ERR_SUCCESS;
}

void ImuService_Close(ImuService* s)
{
    if (s == NULL) { return; }
    s->inited = false;
    s->yawValid = false;
    s->gyroValid = false;
    s->lastRawValid = false;
    publish(s);
    sh2_close();
}

void ImuService_Update(ImuService* s)
{
    if (s == NULL || !s->inited) { return; }
    /* I2C and processing use task-private fields. Readers keep consuming the
     * previous published sample while this slow work is in progress. */
    for (unsigned i = 0; i < 8u; ++i) { sh2_service(); }
    if (s->yawResetRequested)
    {
        s->referenceId = ++s_referenceId;
        s->yawDeg = 0.0f;
        s->yawResetRequested = false;
    }
    if (s->processedGrvCount != s->grvCount)
    {
        s->processedGrvCount = s->grvCount;
        const float norm2 = s->quatW*s->quatW + s->quatX*s->quatX +
                            s->quatY*s->quatY + s->quatZ*s->quatZ;
        s->yawValid = isfinite(norm2) && norm2 >= 0.5f && norm2 <= 1.5f;
        if (s->yawValid)
        {
            const float inv = 1.0f / sqrtf(norm2);
            const float w=s->quatW*inv, x=s->quatX*inv;
            const float y=s->quatY*inv, z=s->quatZ*inv;
            const float raw = atan2f(2.0f*(w*z+x*y), 1.0f-2.0f*(y*y+z*z))*IMU_RAD2DEG;
            s->yawValid = isfinite(raw);
            if (s->yawValid)
            {
                if (!s->lastRawValid) { s->yawDeg = raw; }
                else { s->yawDeg += remainderf(raw - s->lastRawYawDeg, 360.0f); }
                s->lastRawYawDeg = raw;
                s->lastRawValid = true;
                s->lastUpdateMs = HAL_GetTick();
            }
        }
    }
    publish(s);
}

void ImuService_ResetYawReference(ImuService* s)
{
    if (s != NULL) { s->yawResetRequested = true; }
}

bool ImuService_GetSnapshot(ImuService* s, ImuSnapshot* out)
{
    if (s == NULL || out == NULL) { return false; }
    const uint32_t key = control_critical_enter();
    *out = s->published;
    control_critical_exit(key);
    return true;
}
