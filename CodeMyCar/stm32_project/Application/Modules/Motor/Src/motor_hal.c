/*
 * motor_hal.c
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */


#include <string.h>
#include "motor_hal.h"

#include "cmd_process.h"
#include "log_manager.h"

// 内部宏定义
#define DUTY_CLAMP_MIN (-100.0f)
#define DUTY_CLAMP_MAX (100.0f)
#define ENCODER_16BIT_MAX (0xFFFF)  // 16位编码器最大值
#define ENCODER_32BIT_MAX (0xFFFFFFFF) // 32位编码器最大值


/**
 * @brief 安全限制占空比在有效范围内
 *
 * @param value 输入占空比
 * @return float 限制后的占空比
 */
static inline float clamp_duty(const float value)
{
    if (value > DUTY_CLAMP_MAX) return DUTY_CLAMP_MAX;
    if (value < DUTY_CLAMP_MIN) return DUTY_CLAMP_MIN;
    return value;
}

/**
 * 相关配置已经通过 CubeMX 完成配置，这里只需要实现启动逻辑即可
 */
HAL_StatusTypeDef HAL_Motor_Init(HAL_MotorConfig* config)
{
    // 参数校验
    if (!config)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return HAL_ERROR;
    }

    if (config->pwm.tim == NULL || config->encoder.tim == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "PWM or Encoder timer not configured");
        return HAL_ERROR;
    }

    // 启动PWM输出
    HAL_StatusTypeDef status = HAL_TIM_PWM_Start(config->pwm.tim, config->pwm.ch);
    if (status != HAL_OK)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "PWM start failed: %d", status);
        return status;
    }

    // 启动编码器接口
    status = HAL_TIM_Encoder_Start(config->encoder.tim, TIM_CHANNEL_ALL);
    if (status != HAL_OK)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Encoder start failed: %d", status);
        return status;
    }

    // 初始计数器归零
    __HAL_TIM_SET_COUNTER(config->encoder.tim, 0);

    // 初始化GPIO默认状态（滑行停止）
    HAL_GPIO_WritePin(config->gpio.in1Port, config->gpio.in1Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(config->gpio.in2Port, config->gpio.in2Pin, GPIO_PIN_RESET);

    // 初始化状态结构体
    memset(&config->state, 0, sizeof(HAL_MotorState));
    config->state.direction = MOTOR_COAST_STOP;

    return HAL_OK;
}

void HAL_Motor_SetDirection(HAL_MotorConfig* config, const MotorDirection direction)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor config is NULL");
        return;
    }

    // 仅当方向改变时更新
    if (config->state.direction == direction)
    {
        return;
    }

    // 更新方向状态
    config->state.direction = direction;

    switch (direction)
    {
        case MOTOR_DIR_FORWARD:
            HAL_GPIO_WritePin(config->gpio.in1Port, config->gpio.in1Pin, GPIO_PIN_SET);
            HAL_GPIO_WritePin(config->gpio.in2Port, config->gpio.in2Pin, GPIO_PIN_RESET);
            break;

        case MOTOR_DIR_BACKWARD:
            HAL_GPIO_WritePin(config->gpio.in1Port, config->gpio.in1Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(config->gpio.in2Port, config->gpio.in2Pin, GPIO_PIN_SET);
            break;
        case MOTOR_COAST_STOP:
            HAL_GPIO_WritePin(config->gpio.in1Port, config->gpio.in1Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(config->gpio.in2Port, config->gpio.in2Pin, GPIO_PIN_RESET);
            break;

        case MOTOR_BRAKE_STOP:
            HAL_GPIO_WritePin(config->gpio.in1Port, config->gpio.in1Pin, GPIO_PIN_SET);
            HAL_GPIO_WritePin(config->gpio.in2Port, config->gpio.in2Pin, GPIO_PIN_SET);
            break;
        default:
            LOG_WARN(LOG_MODULE_MOTOR, "Invalid direction: %d", direction);
            break;
    }
}

void HAL_Motor_EmergencyStop(HAL_MotorConfig* config)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return;
    }

    // 立即关闭PWM输出
    __HAL_TIM_SET_COMPARE(config->pwm.tim, config->pwm.ch, 0);

    // 激活硬件刹车模式
    HAL_Motor_SetDirection(config, MOTOR_BRAKE_STOP);

    // 复位编码器
    __HAL_TIM_SET_COUNTER(config->encoder.tim, 0);
    config->state.encoderTicks = 0;

    // 更新状态
    config->state.currentDuty = 0;
}

void HAL_Motor_SetDuty(HAL_MotorConfig* config, const float duty)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return;
    }

    // 占空比限幅
    const float clampedDuty = clamp_duty(duty);

    // 获取定时器的 ARR（PWM 周期）
    const uint32_t arr = __HAL_TIM_GET_AUTORELOAD(config->pwm.tim);

    // 计算 CCR 值（四舍五入优化）
    const float    absDuty = fabsf(clampedDuty);
    const uint32_t ccr     = (uint32_t)((absDuty * (float)arr / 100.0f) + 0.5f);

    // 确保 CCR 不超过 ARR
    const uint32_t safeCCR = (ccr > arr) ? arr : ccr;

    // 设置方向
    MotorDirection newDirection;
    if (fabsf(clampedDuty) < 0.1f) // 接近零
    {
        newDirection = MOTOR_COAST_STOP;
    }
    else if (clampedDuty > 0)
    {
        newDirection = MOTOR_DIR_FORWARD;
    }
    else
    {
        newDirection = MOTOR_DIR_BACKWARD;
    }

    // 仅当方向改变时更新
    if (config->state.direction != newDirection)
    {
        HAL_Motor_SetDirection(config, newDirection);
    }

    // 更新 PWM 比较值
    __HAL_TIM_SET_COMPARE(config->pwm.tim, config->pwm.ch, safeCCR);

    // 更新状态
    config->state.currentDuty = (int16_t)clampedDuty;;
}

int32_t HAL_Motor_ReadEncoder(HAL_MotorConfig* config)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return 0;
    }
    // 读取当前编码器计数值
    const uint32_t currentTicks = __HAL_TIM_GET_COUNTER(config->encoder.tim);

    // 获取编码器最大计数值
    const uint32_t maxValue = HAL_Motor_GetEncoderMaxValue(config);

    const uint32_t halfMax = maxValue / 2;

    // 计算增量（处理溢出）
    const uint32_t lastRawTicks = config->state.encoderTicks & maxValue;
    int32_t        delta        = (int32_t)currentTicks - (int32_t)lastRawTicks;

    // 处理计数器溢出（双向）
    if (delta > halfMax)
    {
        // 正向溢出修正：实际是反向大位移
        delta -= (int32_t)(maxValue + 1);
    }
    else if (delta < -(int32_t)halfMax)
    {
        // 反向溢出修正：实际是正向大位移
        delta += (int32_t)(maxValue + 1);
    }

    // 更新累计值
    config->state.encoderTicks += delta;
    return config->state.encoderTicks;
}

uint32_t HAL_Motor_ReadRawEncoder(HAL_MotorConfig* config)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return 0;
    }
    return __HAL_TIM_GET_COUNTER(config->encoder.tim);
}

HAL_MotorState HAL_Motor_GetStatus(const HAL_MotorConfig* config)
{
    if (config == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL MotorConfig is NULL");
        return (HAL_MotorState){0};
    }
    return config->state; // 返回状态结构体副本 (值返回机制：结构体值拷贝而非指针)
}

uint32_t HAL_Motor_GetEncoderMaxValue(const HAL_MotorConfig* config)
{
    const TIM_HandleTypeDef* tim = config->encoder.tim;
    if (tim->Instance->ARR == 0)
    {
        // 自动重装载寄存器未设置，使用默认值
        return (tim->Init.Period > 0) ? tim->Init.Period : ENCODER_16BIT_MAX;
    }
    return tim->Instance->ARR;
}
