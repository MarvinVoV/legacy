/*
 * motor_hal.h
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */


#ifndef MODULES_MOTOR_INC_MOTOR_HAL_H_
#define MODULES_MOTOR_INC_MOTOR_HAL_H_

#include "stm32h7xx_hal.h"

/**
 * @brief 电机运行方向控制模式
 */
typedef enum
{
    MOTOR_DIR_FORWARD  = 0x01, ///< 正转 (IN1=高, IN2=低)
    MOTOR_DIR_BACKWARD = 0x02, ///< 反转 (IN1=低, IN2=高)
    MOTOR_COAST_STOP   = 0x04, ///< 滑行停止 (IN1=低, IN2=低)
    MOTOR_BRAKE_STOP   = 0x08  ///< 刹车停止 (IN1=高, IN2=高)
} MotorDirection;


/**
 * @brief 电机实时状态结构
 */
typedef struct
{
    uint16_t       currentDuty;  ///< 当前PWM占空比 (0-100%)
    MotorDirection direction;    ///< 当前方向状态
    int32_t        encoderTicks; ///< 编码器累计脉冲数
} HAL_MotorState;

/**
 * @brief 电机硬件配置结构
 */
typedef struct
{
    /* PWM控制接口 */
    struct
    {
        TIM_HandleTypeDef* tim; ///< PWM定时器句柄 (如TIM3)
        uint32_t           ch;  ///< PWM输出通道 (TIM_CHANNEL_x)
    } pwm;

    /* 编码器接口 */
    struct
    {
        TIM_HandleTypeDef* tim; ///< 编码器定时器句柄 (如TIM4)
    } encoder;

    /* 驱动控制引脚 */
    struct
    {
        GPIO_TypeDef* in1Port; ///< IN1控制引脚GPIO端口 (如GPIOA)
        uint16_t      in1Pin;  ///< IN1控制引脚编号 (如GPIO_PIN_4)
        GPIO_TypeDef* in2Port; ///< IN2控制引脚GPIO端口 (如GPIOB)
        uint16_t      in2Pin;  ///< IN2控制引脚编号 (如GPIO_PIN_5)
    } gpio;

    HAL_MotorState state; ///< 电机状态结构体
} HAL_MotorConfig;

/**
 * @brief 初始化电机硬件接口
 *
 * @param[in] config 电机配置结构体指针
 *
 * @retval HAL_OK    初始化成功
 * @retval HAL_ERROR 初始化失败
 *
 * @note 此函数执行以下操作：
 *       1. 启动PWM输出
 *       2. 启动编码器接口
 *       3. 复位GPIO控制引脚
 *       4. 初始化状态结构体
 */
HAL_StatusTypeDef HAL_Motor_Init(HAL_MotorConfig* config);

/**
 * @brief 设置电机PWM占空比
 *
 * @param[in] config 电机配置结构体指针
 * @param[in] duty   目标占空比 (-100.0 ~ +100.0)
 *
 * @note 占空比符号决定方向：
 *       - 正值：正转
 *       - 负值：反转
 *       - 零值：滑行停止
 */
void HAL_Motor_SetDuty(HAL_MotorConfig* config, float duty);

/**
 * @brief 设置电机运行方向
 *
 * @param[in] config     电机配置结构体指针
 * @param[in] direction  目标方向 (MotorDirection枚举)
 */
void HAL_Motor_SetDirection(HAL_MotorConfig* config, MotorDirection direction);

/**
 * @brief 电机紧急停止
 *
 * @param[in] config 电机配置结构体指针
 *
 * @note 此函数执行以下操作：
 *       1. 设置占空比为0%
 *       2. 激活刹车模式
 *       3. 复位编码器计数器
 */
void HAL_Motor_EmergencyStop(HAL_MotorConfig* config);

/**
 * @brief 读取编码器累计脉冲值
 *
 * @param[in] config 电机配置结构体指针
 * @return int32_t 编码器累计脉冲数
 *
 * @note 此函数自动处理计数器溢出
 */
int32_t HAL_Motor_ReadEncoder(HAL_MotorConfig* config);

/**
 * @brief 读取编码器原始脉冲值
 * @param config   电机配置结构体指针
 * @return uint32_t 编码器原始脉冲数
 */
uint32_t HAL_Motor_ReadRawEncoder(HAL_MotorConfig* config);

/**
 * @brief 获取电机当前状态
 *
 * @param[in] config 电机配置结构体指针
 * @return HAL_MotorState 电机状态结构体副本
 *
 * @warning 返回的是状态副本，非实时状态指针
 */
HAL_MotorState HAL_Motor_GetStatus(const HAL_MotorConfig* config);


/**
 * @brief 获取编码器最大值
 *
 * @param[in] config 电机配置结构体指针
 * @return uint32_t 编码器最大值
 */
uint32_t HAL_Motor_GetEncoderMaxValue(const HAL_MotorConfig* config);

#endif /* MODULES_MOTOR_INC_MOTOR_HAL_H_ */
