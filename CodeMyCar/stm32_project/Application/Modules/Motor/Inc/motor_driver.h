/*
 * motor_driver.h
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */

#ifndef MODULES_MOTOR_INC_MOTOR_DRIVER_H_
#define MODULES_MOTOR_INC_MOTOR_DRIVER_H_

#include <math.h>
#include <stdbool.h>
#include "freertos_os2.h"
#include "pid_controller.h"
#include "motor_hal.h"

//------------------------------------------------------------------------------
// 类型定义
//------------------------------------------------------------------------------

/**
 * @brief 电机运行模式
 */
typedef enum
{
    MOTOR_DRIVER_MODE_STOP        = 0x01, ///< 停止模式（立即停止）
    MOTOR_DRIVER_MODE_OPEN_LOOP   = 0x02, ///< 开环控制（直接设置占空比）
    MOTOR_DRIVER_MODE_CLOSED_LOOP = 0x04  ///< 闭环控制（PID速度控制）
} MotorDriverMode;


/**
 * @brief 电机物理规格参数
 */
typedef struct
{
    uint16_t encoderPPR;    ///< 编码器每转脉冲数（原始值，未计算倍频）
    uint16_t gearRatio;     ///< 减速比（如30:1存储为30）
    float    wheelRadiusMM; ///< 轮半径（毫米）
    float    maxRPM;        ///< 最大转速（转/分钟）
} MotorSpec;

/**
 * @brief 电机实时状态
 */
typedef struct
{
    struct
    {
        float radps; ///< 当前角速度 [rad/s]，即电机输出轴（即经过减速后的最终输出轴）的角速度
        float mps;   ///< 当前线速度 [m/s]，轮子边缘的线速度
        float rpm;   ///< 当前转速 [RPM]，即电机输出轴（即经过减速后的最终输出轴）的转速
    } velocity;

    struct
    {
        int32_t  totalPulses;   ///< 累计脉冲数
        float    revolutions;   ///< 累计转数
        uint32_t lastRawPulses; ///< 上次原始脉冲值
    } position;

    struct
    {
        uint32_t lastLogPrint;   ///<< 每个电机独立的日志时间戳
        float    logElapsedTime; ///<< 每个电机独立的时间累积
    } debug;

    float           openLoopDuty;    ///< 当前开环占空比 [%]
    uint32_t        lastUpdateTick;  ///< 上次更新时间戳 [ms]
    uint32_t        lastControlTick; ///< 控制执行时间戳[ms]
    MotorDriverMode mode;            ///< 当前控制模式
    MotorDirection  direction;       ///< 当前电机运动方向
} MotorDriverState;

/**
 * @brief 电机驱动控制器
 */
typedef struct
{
    HAL_MotorConfig* halCfg; ///< 硬件抽象层配置
    const MotorSpec* spec;   ///< 电机规格参数
    uint8_t          id;     ///< 电机ID

    struct
    {
        MotorDriverMode mode;      ///< 控制模式
        MotorDirection  direction; ///< 方向
        float           targetRPM; ///< 目标转速 [RPM],表示电机输出轴（即经过减速后的最终输出轴）的转速
        PID_Controller  pidCtrl;   ///< PID控制器实例（非指针）
    } control;

    MotorDriverState state; ///< 实时状态

    struct
    {
        uint8_t windowSize;   ///< 速度滤波窗口大小
        float*  filterBuffer; ///< 滤波缓冲区
        uint8_t bufferIndex;  ///< 缓冲区索引
        float   lpf_state;    ///< 低通滤波状态
        // PWM输出滤波
        float pwm_filter_state;
        float pwm_alpha;
    } filter;
} MotorDriver;

//------------------------------------------------------------------------------
// 公共API
//------------------------------------------------------------------------------

/**
 * @brief 初始化电机驱动控制器
 *
 * @param driver 驱动控制器实例
 * @param initialPid 初始PID参数
 *
 * @retval true  初始化成功
 * @retval false 初始化失败
 *
 * @note 执行以下操作：
 *       1. 初始化PID控制器
 *       2. 分配滤波缓冲区
 *       3. 重置状态变量
 */
bool MotorDriver_Init(MotorDriver* driver, const PID_Params* initialPid);

/**
 * @brief 设置电机控制模式
 *
 * @param driver 驱动控制器实例
 * @param mode 目标控制模式
 *
 * @note 模式切换行为：
 *       - STOP模式：立即停止电机
 *       - OPEN_LOOP：保留当前占空比
 *       - CLOSED_LOOP：重置PID积分项
 */
void MotorDriver_SetMode(MotorDriver* driver, MotorDriverMode mode);

/**
 * @brief 设置目标转速（闭环模式）
 *
 * @param driver 驱动控制器实例
 * @param rpm 目标转速 [RPM]
 *
 * @note 自动限制在规格允许范围内
 */
void MotorDriver_SetTargetRPM(MotorDriver* driver, float rpm);

/**
 * @brief 设置开环占空比
 *
 * @param driver 驱动控制器实例
 * @param duty 占空比 [-100.0, 100.0]
 *
 * @note 仅在开环模式下有效
 */
void MotorDriver_SetDutyCycle(MotorDriver* driver, float duty);


/**
 * @brief 更新电机控制状态
 *
 * @param driver 驱动控制器实例
 *
 * @note 应周期性调用（5-20ms）
 */
void MotorDriver_Update(MotorDriver* driver);

/**
 * @brief 获取当前电机状态
 *
 * @param driver 驱动控制器实例
 * @return MotorDriverState 状态副本
 */
MotorDriverState MotorDriver_getState(const MotorDriver* driver);
/**
 * @brief 执行紧急停止
 *
 * @param driver 驱动控制器实例
 *
 * @note 执行以下操作：
 *       1. 切断PWM输出
 *       2. 激活硬件刹车
 *       3. 切换到STOP模式
 */
void MotorDriver_EmergencyStop(MotorDriver* driver);

/**
 * @brief 配置PID参数
 *
 * @param driver 驱动控制器实例
 * @param params PID参数
 */
void MotorDriver_ConfigurePID(MotorDriver* driver, const PID_Params* params);


#endif /* MODULES_MOTOR_INC_MOTOR_DRIVER_H_ */
