/*
 * motor_service.h
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */

#ifndef SERVICES_MOTOR_INC_MOTOR_SERVICE_H_
#define SERVICES_MOTOR_INC_MOTOR_SERVICE_H_
#include <stdint.h>

#include "cmsis_os2.h"
#include "motor_driver.h"
#include "pid_controller.h"

//------------------------------------------------------------------------------
// 类型定义
//------------------------------------------------------------------------------


/**
 * @brief 电机标识符
 */
typedef enum
{
    MOTOR_LEFT_WHEEL  = 0, ///< 左轮电机
    MOTOR_RIGHT_WHEEL = 1, ///< 右轮电机
    MOTOR_MAX_NUM     = 2  ///< 最大电机数量
} MotorID;


/**
 * @brief 电机实时状态
 */
typedef struct
{
    struct
    {
        float mps; ///< 当前线速度 (m/s)
        float rpm; ///< 当前转速 (RPM)
    } velocity;

    struct
    {
        float revolutions; ///< 累计转数
        float odometer;    ///< 累计里程 (m)
    } position;

    float           openLoopDuty; ///< 当前开环占空比 (0.0~1.0)
    MotorDriverMode mode;         ///< 当前控制模式

    uint32_t lastUpdate; ///< 上次更新时间戳 (ms)
} MotorState;


/**
 * @brief 电机实例
 */
typedef struct
{
    MotorDriver* driver;        ///< 电机驱动实例
    MotorState   state;         ///< 状态信息
    osMutexId_t  mutex;         ///< 实例级互斥锁
    uint8_t      isInitialized; ///< 初始化标志
} MotorInstance;


/**
 * @brief 电机服务控制器
 */
typedef struct
{
    MotorInstance instances[MOTOR_MAX_NUM]; ///< 电机实例数组
    osThreadId_t  leftInstanceTaskHandle;   ///< Left任务句柄
    osThreadId_t  rightInstanceTaskHandle;  ///< Right任务句柄
} MotorService;


//------------------------------------------------------------------------------
// 公共API
//------------------------------------------------------------------------------


/**
 * @brief 初始化电机服务
 *
 * @param[in] service 服务实例
 * @param[in] leftDriver 左轮驱动实例
 * @param[in] rightDriver 右轮驱动实例
 *
 * @retval ERR_SUCCESS 初始化成功
 * @retval ERR_MOTOR_INVALID_PARAM 无效参数
 * @retval ERR_MOTOR_MUTEX_FAIL 互斥锁创建失败
 * @retval ERR_MOTOR_DRIVER_INIT_FAIL 驱动初始化失败
 *
 * @note 执行以下操作：
 *       1. 初始化服务结构体
 *       2. 创建互斥锁
 *       3. 初始化电机驱动
 *       4. 启动状态更新任务
 */
int MotorService_init(MotorService* service, MotorDriver* leftDriver, MotorDriver* rightDriver);

/**
 * @brief 设置目标转速
 *
 * @param[in] service 服务实例
 * @param[in] motorId 电机ID
 * @param[in] rpm 目标转速 (RPM)
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTOR_INVALID_PARAM 无效参数
 * @retval ERR_MOTOR_MUTEX_TIMEOUT 互斥锁超时
 *
 * @note 自动切换到闭环控制模式
 */
int MotorService_setTargetRPM(MotorService* service, MotorID motorId, float rpm);


/**
 * @brief 设置开环占空比
 *
 * @param[in] service 服务实例
 * @param[in] motorId 电机ID
 * @param[in] dutyCycle 占空比 (-1.0~1.0)
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTOR_INVALID_PARAM 无效参数
 * @retval ERR_MOTOR_MUTEX_TIMEOUT 互斥锁超时
 * @retval ERR_MOTOR_DUTY_OUT_OF_RANGE 占空比超限
 *
 * @note 自动切换到开环控制模式
 */
int MotorService_setOpenLoopDutyCycle(MotorService* service, MotorID motorId, float dutyCycle);

/**
 * @brief 执行紧急停止
 *
 * @param[in] service 服务实例
 * @param[in] motorId 电机ID
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTOR_INVALID_PARAM 无效参数
 * @retval ERR_MOTOR_MUTEX_TIMEOUT 互斥锁超时
 *
 * @note 立即切断电机输出并锁定状态
 */
int MotorService_emergencyStop(MotorService* service, MotorID motorId);


/**
 * @brief 更新所有电机状态
 *
 * @param instance 服务实例
 *
 * @note 应在控制循环中周期性调用
 */
void MotorService_updateState(MotorInstance* instance);

/**
 * @brief 获取电机状态
 *
 * @param[in] service 服务实例
 * @param[in] motorId 电机ID
 * @return MotorState 状态副本
 *
 * @note 返回状态副本，线程安全
 */
MotorState MotorService_getState(MotorService* service, MotorID motorId);

/**
 * @brief 配置PID参数
 *
 * @param[in] service 服务实例
 * @param[in] motorId 电机ID
 * @param[in] pidParams PID参数
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTOR_INVALID_PARAM 无效参数
 * @retval ERR_MOTOR_MUTEX_TIMEOUT 互斥锁超时
 */
int MotorService_configurePID(MotorService* service, MotorID motorId, const PID_Params* pidParams);


#endif /* SERVICES_MOTOR_INC_MOTOR_SERVICE_H_ */
