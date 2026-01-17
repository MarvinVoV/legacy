/*
 * servo_driver.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef MODULES_SERVO_INC_SERVO_DRIVER_H_
#define MODULES_SERVO_INC_SERVO_DRIVER_H_

#include "servo_hal.h"

// 定义舵机相关参数

#define SERVO_ANGLE_MIN (0)     // 最小角度 (0°)
#define SERVO_ANGLE_MAX (180)   // 最大角度 (180°)


typedef struct
{
    HAL_ServoConfig* hw; // 硬件配置
    struct
    {
        float minAngle;      // 最小角度（度）
        float maxAngle;      // 最大角度（度）
        uint16_t minPulseUs; // 最小对应脉冲（μs）
        uint16_t maxPulseUs; // 最大对应脉冲（μs）
        float resolutionDeg; // 角度控制分辨率（度/步）
    } spec;
} ServoDriver;

/**
 * 初始化
 * @param driver driver
 */
int ServoDriver_Init(ServoDriver* driver);
/**
 * @brief 直接设置舵机角度（立即生效，无平滑过程）
 * @param driver driver
 * @param angleDeg 目标角度（SERVO_ANGLE_MIN~SERVO_ANGLE_MAX）
 */
int ServoDriver_setAngle(ServoDriver* driver, float angleDeg);

/**
 * @brief 紧急停止
 * @param driver driver
 */
int ServoDriver_emergencyStop(const ServoDriver* driver);

#endif /* MODULES_SERVO_INC_SERVO_DRIVER_H_ */
