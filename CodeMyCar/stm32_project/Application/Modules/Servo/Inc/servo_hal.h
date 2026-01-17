/*
 * servo_hal.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef MODULES_SERVO_INC_SERVO_HAL_H_
#define MODULES_SERVO_INC_SERVO_HAL_H_

#include "stm32h7xx_hal.h"
#include "log_manager.h"

#define SERVO_MIN_PULSE (500)   // 最小脉冲宽度 (500us)
#define SERVO_MAX_PULSE (2500)  // 最大脉冲宽度 (2500us)

typedef struct
{
    TIM_HandleTypeDef* pwmTim; // PWM定时器句柄
    uint32_t channel;          // PWM通道
    uint16_t minPulse;         // 最小有效脉冲宽度（例：500μs）
    uint16_t maxPulse;         // 最大有效脉冲宽度（例：2500μs）
    uint16_t safetyPulse;      // 安全位置脉冲（紧急停止时使用）
} HAL_ServoConfig;

/**
 * @brief 初始化舵机硬件层
 * @param config 硬件配置参数（必须已正确配置定时器）
 */
int ServoHAL_init(HAL_ServoConfig* config);


/**
 * 设置舵机脉冲宽度（单位：微秒）
 * @param config   HAL_ServoConfig 实例
 * @param pulseUs  目标脉冲宽度（μs）
 * @return         错误码
 */
int ServoHAL_setPulse(HAL_ServoConfig* config, uint16_t pulseUs);

/**
 * 安全停止接口
 */
int ServoHAL_emergencyStop(HAL_ServoConfig* config);

#endif /* MODULES_SERVO_INC_SERVO_HAL_H_ */
