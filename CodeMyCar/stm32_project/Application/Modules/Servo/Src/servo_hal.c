/*
 * servo_drive.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#include "servo_hal.h"
#include "error_code.h"
#include "macros.h"

static uint32_t convertUsToTicks(TIM_HandleTypeDef* tim, uint16_t microseconds);

int ServoHAL_init(HAL_ServoConfig* config)
{
    if (!config || !config->pwmTim)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid servo config");
        return ERR_HAL_SERVO_INVALID_ARG;
    }
    // 脉冲参数验证
    if (config->minPulse >= config->maxPulse)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid pulse range: min=%d, max=%d",
                  config->minPulse, config->maxPulse);
        return ERR_HAL_SERVO_INVALID_ARG;
    }

    // 校验脉冲范围有效性
    const uint32_t arr = __HAL_TIM_GET_AUTORELOAD(config->pwmTim);
    const uint32_t minTicks = convertUsToTicks(config->pwmTim, config->minPulse);
    if (minTicks > arr)
    {
        LOG_WARN(LOG_MODULE_SERVO, "Pulse exceeds ARR: %lu > %lu", minTicks, arr);
        return ERR_HAL_SERVO_FAILURE;
    }

    if (config->minPulse == 0) config->minPulse = SERVO_MIN_PULSE;
    if (config->maxPulse == 0) config->maxPulse = SERVO_MAX_PULSE;

    // 安全脉冲配置
    if (config->safetyPulse == 0)
    {
        config->safetyPulse = (config->minPulse + config->maxPulse) / 2;
    }

    __HAL_TIM_SET_COMPARE(config->pwmTim, config->channel, config->safetyPulse);

    // 启动PWM并验证
    if (HAL_TIM_PWM_Start(config->pwmTim, config->channel) != HAL_OK)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "PWM start failed");
        return ERR_HAL_SERVO_FAILURE;
    }
    return ERR_SUCCESS;
}

int ServoHAL_setPulse(HAL_ServoConfig* config, const uint16_t pulseUs)
{
    if (!config || !config->pwmTim)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid servo config");
        return ERR_HAL_SERVO_INVALID_ARG;
    }

    // 脉冲宽度限幅
    const uint16_t clampedPulse = CLAMP(pulseUs, config->minPulse, config->maxPulse);

    // 转换为定时器时钟周期数
    const uint32_t targetTicks = convertUsToTicks(config->pwmTim, clampedPulse);

    // 原子操作设置比较值
    __HAL_TIM_SET_COMPARE(config->pwmTim, config->channel, targetTicks);
    return ERR_SUCCESS;
}

int ServoHAL_emergencyStop(HAL_ServoConfig* config)
{
    if (!config || !config->pwmTim)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid servo config");
        return ERR_HAL_SERVO_INVALID_ARG;
    }

    // 立即停止PWM输出
    HAL_TIM_PWM_Stop(config->pwmTim, config->channel);

    // 设置安全位置脉冲
    __HAL_TIM_SET_COMPARE(config->pwmTim, config->channel, convertUsToTicks(config->pwmTim, config->safetyPulse));

    // 重新启动PWM
    if (HAL_TIM_PWM_Start(config->pwmTim, config->channel) != HAL_OK)
    {
        return ERR_HAL_SERVO_FAILURE;
    }
    return ERR_SUCCESS;
}

static uint32_t convertUsToTicks(TIM_HandleTypeDef* tim, uint16_t microseconds)
{
    // 1. 获取定时器时钟频率（单位：Hz）
    uint32_t timerClockHz = HAL_RCC_GetPCLK1Freq();

    // 2. 检查是否需要应用 APB1 预分频补偿
#if defined(TIM2) || defined(TIM3) || defined(TIM4) || defined(TIM5)
    if (IS_TIM_APB1_INSTANCE(tim->Instance))
    {
        timerClockHz *= 2; // APB1 定时器时钟频率翻倍
    }
#endif

    // 3. 计算预分频后的实际时钟频率
    const uint32_t preScaler = tim->Instance->PSC + 1;
    const uint32_t timerActualClockHz = timerClockHz / preScaler;

    // 4. 计算 ticks（使用整数运算）
    return (microseconds * timerActualClockHz) / 1000000U;
}
