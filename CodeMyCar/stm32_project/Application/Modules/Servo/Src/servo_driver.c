/*
 * servo_driver.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#include <stdlib.h>
#include "cmsis_os2.h"
#include "servo_driver.h"
#include "error_code.h"
#include "log_manager.h"
#include "macros.h"

int ServoDriver_Init(ServoDriver* driver)
{
    if (driver == NULL || driver->hw == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_Init: driver is NULL");
        return ERR_HAL_SERVO_INVALID_ARG;
    }

    // 参数默认值
    driver->spec.minPulseUs = driver->hw->minPulse;
    driver->spec.maxPulseUs = driver->hw->maxPulse;

    /* --------------------- 规格校验 --------------------- */
    if (driver->spec.maxAngle <= driver->spec.minAngle)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid angle range [%.1f, %.1f]",
                  driver->spec.minAngle, driver->spec.maxAngle);
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    if (driver->spec.maxPulseUs <= driver->spec.minPulseUs)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid pulse range [%uus, %uus]",
                  driver->spec.minPulseUs, driver->spec.maxPulseUs);
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    if (driver->spec.resolutionDeg <= 0.0f)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Resolution must > 0, got %.2f", driver->spec.resolutionDeg);
        return ERR_HAL_SERVO_INVALID_ARG;
    }

    // 初始化舵机硬件
    const int halStatus = ServoHAL_init(driver->hw);
    if (halStatus != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "HAL init failed: %d", halStatus);
        return halStatus;
    }
    return ERR_SUCCESS;
}

int ServoDriver_setAngle(ServoDriver* driver, const float angleDeg)
{
    if (driver == NULL || driver->hw == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_setAngle: driver is NULL");
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    if (driver->spec.maxPulseUs <= driver->spec.minPulseUs)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid pulse range [%uus, %uus]",
                  driver->spec.minPulseUs, driver->spec.maxPulseUs);
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    // 角度限幅 & 量化
    const float clamped = fmaxf(fminf(angleDeg, driver->spec.maxAngle),
                                driver->spec.minAngle);
    float quantized = roundf(clamped / driver->spec.resolutionDeg)
        * driver->spec.resolutionDeg;

    // 量化后再次钳位，避免精度溢出
    quantized = fmaxf(fminf(quantized, driver->spec.maxAngle), driver->spec.minAngle);

    // 脉冲计算
    const float pulseUs = (float)driver->spec.minPulseUs +
        (quantized - driver->spec.minAngle) *
        (float)(driver->spec.maxPulseUs - driver->spec.minPulseUs) /
        (driver->spec.maxAngle - driver->spec.minAngle);

    const uint16_t safePulseUs = (uint16_t)CLAMP(pulseUs + 0.5f,
                                                 driver->spec.minPulseUs,
                                                 driver->spec.maxPulseUs);
    // 硬件操作
    const int halStatus = ServoHAL_setPulse(driver->hw, safePulseUs);
    if (halStatus != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "HAL layer error: %d", halStatus);
        return ERR_HAL_SERVO_FAILURE;
    }
    return ERR_SUCCESS;
}

int ServoDriver_emergencyStop(const ServoDriver* driver)
{
    if (driver == NULL || driver->hw == NULL)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_emergencyStop: driver is NULL");
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    const int halStatus = ServoHAL_emergencyStop(driver->hw);
    if (halStatus != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Emergency stop failed: %d", halStatus);
    }
    return halStatus;
}
