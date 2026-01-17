/*
 * motor_service.c
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "motor_service.h"
#include "error_code.h"
#include "log_manager.h"
#include "motor_task.h"
#include "pid_controller.h"

//------------------------------------------------------------------------------
// 常量定义
//------------------------------------------------------------------------------

/// 互斥锁超时时间 (ms)
#define MUTEX_TIMEOUT_MS (50)
/// 最小有效占空比
#define MIN_DUTY_CYCLE (-1.0f)
/// 最大有效占空比
#define MAX_DUTY_CYCLE (1.0f)

//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------

/**
 * @brief 计算里程表值
 *
 * @param revolutions 累计转数
 * @param wheelRadiusMM 轮半径 (mm)
 * @return float 里程 (m)
 */
static float calculate_odometer(float revolutions, float wheelRadiusMM);


//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------

int MotorService_init(MotorService* service, MotorDriver* leftDriver, MotorDriver* rightDriver)
{
    // 参数验证
    if (service == NULL || leftDriver == NULL || rightDriver == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }

    // 初始化服务结构体
    memset(service, 0, sizeof(MotorService));

    // 初始化左轮实例
    service->instances[MOTOR_LEFT_WHEEL].driver = leftDriver;
    service->instances[MOTOR_LEFT_WHEEL].mutex  = osMutexNew(NULL);
    if (service->instances[MOTOR_LEFT_WHEEL].mutex == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Left motor mutex creation failed");
        return ERR_MOTOR_MUTEX_FAIL;
    }

    // 初始化右轮实例
    service->instances[MOTOR_RIGHT_WHEEL].driver = rightDriver;
    service->instances[MOTOR_RIGHT_WHEEL].mutex  = osMutexNew(NULL);
    if (service->instances[MOTOR_RIGHT_WHEEL].mutex == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Right motor mutex creation failed");
        // 清理已创建资源
        osMutexDelete(service->instances[MOTOR_LEFT_WHEEL].mutex);
        return ERR_MOTOR_MUTEX_FAIL;
    }

    // 初始化电机驱动
    if (MotorDriver_Init(leftDriver, &leftDriver->control.pidCtrl.params) != true)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Left motor driver init failed");
        return ERR_MOTOR_DRIVER_INIT_FAIL;
    }

    if (MotorDriver_Init(rightDriver, &rightDriver->control.pidCtrl.params) != true)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Right motor driver init failed");
        return ERR_MOTOR_DRIVER_INIT_FAIL;
    }

    // 设置初始模式为停止
    MotorDriver_SetMode(leftDriver, MOTOR_DRIVER_MODE_STOP);
    MotorDriver_SetMode(rightDriver, MOTOR_DRIVER_MODE_STOP);

    // 标记初始化完成
    service->instances[MOTOR_LEFT_WHEEL].isInitialized  = 1;
    service->instances[MOTOR_RIGHT_WHEEL].isInitialized = 1;

    // 创建状态更新任务
    MotorTask_init(service);
    return ERR_SUCCESS;
}

int MotorService_setTargetRPM(MotorService* service, MotorID motorId, float rpm)
{
    // 参数验证
    if (service == NULL || motorId >= MOTOR_MAX_NUM)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }

    MotorInstance* instance = &service->instances[motorId];
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor %d not initialized", motorId);
        return ERR_MOTOR_NOT_INITIALIZED;
    }
    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", motorId);
        return ERR_MOTOR_MUTEX_TIMEOUT;
    }


    MotorDriver* driver = instance->driver;

    // 设置闭环模式
    MotorDriver_SetMode(driver, MOTOR_DRIVER_MODE_CLOSED_LOOP);

    // 设置目标转速
    MotorDriver_SetTargetRPM(driver, rpm);

    // 更新状态
    instance->state.mode = MOTOR_DRIVER_MODE_CLOSED_LOOP;

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

int MotorService_setOpenLoopDutyCycle(MotorService* service, MotorID motorId, float dutyCycle)
{
    // 参数验证
    if (service == NULL || motorId >= MOTOR_MAX_NUM)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }

    MotorInstance* instance = &service->instances[motorId];
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor %d not initialized", motorId);
        return ERR_MOTOR_NOT_INITIALIZED;
    }
    // 占空比范围检查
    if (dutyCycle < MIN_DUTY_CYCLE || dutyCycle > MAX_DUTY_CYCLE)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Duty cycle out of range: %.2f", dutyCycle);
        return ERR_MOTOR_DUTY_OUT_OF_RANGE;
    }

    // 转换为百分比 (-100% ~ 100%)
    const float dutyPercent = dutyCycle * 100.0f;

    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", motorId);
        return ERR_MOTOR_MUTEX_TIMEOUT;
    }

    MotorDriver* driver = instance->driver;

    // 设置开环模式
    MotorDriver_SetMode(driver, MOTOR_DRIVER_MODE_OPEN_LOOP);

    // 设置占空比
    MotorDriver_SetDutyCycle(driver, dutyPercent);

    // 更新状态
    instance->state.mode         = MOTOR_DRIVER_MODE_OPEN_LOOP;
    instance->state.openLoopDuty = dutyCycle;

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

int MotorService_emergencyStop(MotorService* service, MotorID motorId)
{
    // 参数验证
    if (service == NULL || motorId >= MOTOR_MAX_NUM)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }

    MotorInstance* instance = &service->instances[motorId];
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor %d not initialized", motorId);
        return ERR_MOTOR_NOT_INITIALIZED;
    }

    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", motorId);
        return ERR_MOTOR_MUTEX_TIMEOUT;
    }

    // 执行紧急停止
    MotorDriver_EmergencyStop(instance->driver);

    // 更新状态
    instance->state.velocity.rpm = 0.0f;
    instance->state.velocity.mps = 0.0f;
    instance->state.mode         = MOTOR_DRIVER_MODE_STOP;

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

void MotorService_updateState(MotorInstance* instance)
{
    if (instance == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid motor instance");
        return;
    }

    // 跳过未初始化实例
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor instance %d not initialized", instance->driver->id);
        return;
    }
    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", instance->driver->id);
        return;
    }

    // 驱动状态更新
    MotorDriver_Update(instance->driver);

    // 获取驱动状态
    const MotorDriverState driverState = MotorDriver_getState(instance->driver);

    // 更新服务层状态
    instance->state.velocity.rpm         = driverState.velocity.rpm;
    instance->state.velocity.mps         = driverState.velocity.mps;
    instance->state.position.revolutions = driverState.position.revolutions;
    instance->state.position.odometer    = calculate_odometer(
        driverState.position.revolutions,
        instance->driver->spec->wheelRadiusMM
    );
    instance->state.mode       = driverState.mode;
    instance->state.lastUpdate = HAL_GetTick();

    // 开环模式下记录占空比
    if (driverState.mode == MOTOR_DRIVER_MODE_OPEN_LOOP)
    {
        instance->state.openLoopDuty = driverState.openLoopDuty / 100.0f;
    }

    osMutexRelease(instance->mutex);
}

MotorState MotorService_getState(MotorService* service, MotorID motorId)
{
    const MotorState emptyState = {0};
    // 参数验证
    if (service == NULL || motorId >= MOTOR_MAX_NUM)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return emptyState;
    }

    MotorInstance* instance = &service->instances[motorId];
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor %d not initialized", motorId);
        return emptyState;
    }

    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", motorId);
        return emptyState;
    }

    // 创建状态副本
    const MotorState stateCopy = instance->state;

    osMutexRelease(instance->mutex);
    return stateCopy;
}

int MotorService_configurePID(MotorService* service, MotorID motorId, const PID_Params* pidParams)
{
    // 参数验证
    if (service == NULL || motorId >= MOTOR_MAX_NUM || pidParams == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }
    MotorInstance* instance = &service->instances[motorId];
    if (!instance->isInitialized)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor %d not initialized", motorId);
        return ERR_MOTOR_NOT_INITIALIZED;
    }
    // 参数有效性检查
    if (pidParams->kp < 0 || pidParams->ki < 0 || pidParams->kd < 0)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid PID parameters");
        return ERR_MOTOR_INVALID_PARAM;
    }

    // 获取互斥锁
    if (osMutexAcquire(instance->mutex, MUTEX_TIMEOUT_MS) != osOK)
    {
        LOG_WARN(LOG_MODULE_MOTOR, "Mutex timeout for motor %d", motorId);
        return ERR_MOTOR_MUTEX_TIMEOUT;
    }

    // 配置PID参数
    MotorDriver_ConfigurePID(instance->driver, pidParams);

    osMutexRelease(instance->mutex);
    return 0;
}

//------------------------------------------------------------------------------
// 内部函数实现
//------------------------------------------------------------------------------
static float calculate_odometer(const float revolutions, const float wheelRadiusMM)
{
    // 周长 = 2 * π * r (单位:米)
    const float circumference = 2.0f * (float)M_PI * (wheelRadiusMM / 1000.0f);
    // 里程 = 转数 * 周长
    return revolutions * circumference;
}
