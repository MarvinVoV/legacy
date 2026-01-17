/*
 * servo_service.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */
#include "servo_service.h"
#include <error_code.h>
#include "cmsis_os2.h"
#include "log_manager.h"
#include "servo_task.h"

static bool checkPositionReached(const SteerInstance* inst);
static void handleCalibratingState(SteerInstance* inst);
static int handleMovingState(SteerInstance* inst);

int SteerService_init(SteerInstance* steerInstance, SteerConfig* config, ServoDriver* driver)
{
    if (!steerInstance || !config || !driver)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid config or driver");
        return ERR_SRV_SERVO_INVALID_ARG;
    }
    // 初始化配置
    memcpy(&steerInstance->config, config, sizeof(SteerConfig));
    steerInstance->driver = driver;

    // 初始化运行时状态
    steerInstance->state.actualAngleDeg = 0.0f;
    steerInstance->state.targetAngleDeg = 0.0f;
    steerInstance->state.lastUpdateTick = 0;
    steerInstance->state.motionState = STEER_STATE_IDLE;
    steerInstance->state.errorCode = 0;

    // 初始化平滑控制
    steerInstance->smoothCtrl = (SmoothControlContext){
        .targetAngleDeg = 0.0f,
        .currentSpeedDegPerMs = 0.0f,                                      // 初始速度为0
        .accelerationDegPerMs2 = config->accelerationDegPerSec2 / 1000.0f, // 单位转换
        .maxSpeedDegPerMs = config->maxSpeedDegPerSec / 1000.0f,           // 单位转换
        .state = STEER_STATE_IDLE
    };

    // 校准控制初始化
    steerInstance->calibCtrl.calibPhase = 0;
    steerInstance->calibCtrl.calibStartAngle = 0.0f;

    // 创建互斥锁
    steerInstance->mutex = osMutexNew(NULL);
    if (!steerInstance->mutex)
    {
        return ERR_SRV_SERVO_INIT_FAIL;
    }

    // 调用驱动层初始化函数
    if (ServoDriver_Init(steerInstance->driver) != ERR_SUCCESS)
    {
        return ERR_SRV_SERVO_INIT_FAIL;
    }

    // 创建任务
    ServoTask_init(steerInstance);
    return ERR_SUCCESS;
}

int SteerService_setAngleSmoothly(SteerInstance* instance, float targetAngle)
{
    if (!instance || !instance->driver)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid instance or driver");
        return ERR_INVALID_PARAM;
    }
    osMutexAcquire(instance->mutex, osWaitForever);

    /*--------------------- 参数预处理 ---------------------*/
    // 角度限幅（浮点版本）
    const float clampedAngle = fmaxf(instance->config.minAngleDeg,
                                     fminf(targetAngle, instance->config.maxAngleDeg));

    // 计算角度差（浮点）
    const float delta = clampedAngle - instance->state.actualAngleDeg;

    // 死区判断（浮点容差）
    if (fabsf(delta) <= instance->config.deadZoneDeg)
    {
        osMutexRelease(instance->mutex);
        return ERR_SUCCESS;
    }
    // 初始化平滑控制上下文
    instance->smoothCtrl = (SmoothControlContext){
        .targetAngleDeg = clampedAngle,
        .currentSpeedDegPerMs = 0.0f,
        .accelerationDegPerMs2 = instance->config.accelerationDegPerSec2 / 1000.0f, // 转换为度/毫秒²
        .maxSpeedDegPerMs = instance->config.maxSpeedDegPerSec / 1000.0f,           // 转换为度/毫秒
        .state = STEER_STATE_ACCEL
    };

    // 状态机更新
    instance->state.motionState = STEER_MOTION_STATE_MOVING;
    instance->state.lastUpdateTick = HAL_GetTick();

    // 立即触发首次更新
    // SteerService_update(instance);

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

int SteerService_update(SteerInstance* instance)
{
    if (!instance || !instance->driver)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid instance or driver");
        return ERR_INVALID_PARAM;
    }
    osMutexAcquire(instance->mutex, osWaitForever);

    if (instance->state.motionState != STEER_MOTION_STATE_MOVING)
    {
        osMutexRelease(instance->mutex);
        return ERR_SUCCESS;
    }

    // 错误状态优先处理
    if (instance->state.errorCode != 0)
    {
        osMutexRelease(instance->mutex);
        return ERR_SUCCESS;
    }
    /*--------- 状态机处理 ---------*/
    switch (instance->state.motionState)
    {
        case STEER_MOTION_STATE_MOVING:
            handleMovingState(instance);
            break;

        case STEER_MOTION_STATE_CALIBRATING:
            handleCalibratingState(instance);
            break;

        case STEER_MOTION_STATE_IDLE:
        default:
            // 无操作
            break;
    }

    /*--------- 更新最后活动时间 ---------*/
    instance->state.lastUpdateTick = HAL_GetTick();

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

int SteerService_setAngleImmediate(SteerInstance* instance, const float angle)
{
    if (!instance || !instance->driver)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "Invalid instance or driver");
        return ERR_INVALID_PARAM;
    }
    osMutexAcquire(instance->mutex, osWaitForever);
    /*--------------------- 角度处理 ---------------------*/
    // 浮点角度限幅（兼容原整型配置）
    const float minAngle = instance->config.minAngleDeg;
    const float maxAngle = instance->config.maxAngleDeg;
    const float clampedAngle = fmaxf(fminf(angle, maxAngle), minAngle);


    // 计算角度差（浮点）
    const float delta = clampedAngle - instance->state.actualAngleDeg;

    // 死区判断（浮点容差）
    if (fabsf(delta) <= instance->config.deadZoneDeg)
    {
        osMutexRelease(instance->mutex);
        return ERR_SUCCESS;
    }

    // 强制更新实际角度（绕过平滑控制）
    instance->state.targetAngleDeg = clampedAngle;
    instance->state.actualAngleDeg = clampedAngle;

    // 清除平滑控制上下文（适配新状态结构）
    instance->smoothCtrl = (SmoothControlContext){
        .targetAngleDeg = clampedAngle,
        .currentSpeedDegPerMs = 0.0f, // 重置速度
        .state = STEER_STATE_IDLE     // 强制设为空闲状态
    };

    // 立即驱动硬件
    const int err = ServoDriver_setAngle(instance->driver, clampedAngle);
    if (err != ERR_SUCCESS)
    {
        osMutexRelease(instance->mutex);
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_setAngle failed");
        return err;
    }

    // 更新状态
    instance->state.motionState = STEER_STATE_IDLE;
    instance->state.lastUpdateTick = HAL_GetTick();

    osMutexRelease(instance->mutex);
    return ERR_SUCCESS;
}

// 紧急停止
void SteerService_forceStop(SteerInstance* inst)
{
    if (!inst) return;

    osMutexAcquire(inst->mutex, osWaitForever);

    // 停止平滑运动（完全重置控制上下文）
    inst->smoothCtrl = (SmoothControlContext){
        .targetAngleDeg = inst->state.actualAngleDeg, // 保持当前位置
        .currentSpeedDegPerMs = 0.0f,                 // 速度归零
        .state = STEER_STATE_IDLE                     // 设为空闲状态
    };
    // 更新全局状态机
    inst->state.motionState = STEER_STATE_IDLE;

    // 同步硬件角度
    const int err = ServoDriver_emergencyStop(inst->driver);
    if (err != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_emergencyStop failed");
    }

    osMutexRelease(inst->mutex);
}

// 获取当前状态快照（线程安全）
SteerState SteerService_getState(const SteerInstance* inst)
{
    SteerState state = {0};
    if (!inst) return state;

    osMutexAcquire(inst->mutex, osWaitForever);
    memcpy(&state, &inst->state, sizeof(SteerState));
    osMutexRelease(inst->mutex);

    return state;
}

/*--------------------- 运动状态处理函数 ---------------------*/
static int handleMovingState(SteerInstance* inst)
{
    SmoothControlContext* ctrl = &inst->smoothCtrl;
    const float delta = ctrl->targetAngleDeg - inst->state.actualAngleDeg;
    const float remainingDist = fabsf(delta);
    const float direction = (delta >= 0) ? 1.0f : -1.0f;
    // 计算理论停止距离
    const float stopDistance =
        (ctrl->currentSpeedDegPerMs * ctrl->currentSpeedDegPerMs) /
        (2.0f * ctrl->accelerationDegPerMs2);

    // 状态机决策
    if (remainingDist <= stopDistance)
    {
        // 需要减速
        ctrl->state = STEER_STATE_DECEL;
    }
    else if (ctrl->currentSpeedDegPerMs < ctrl->maxSpeedDegPerMs)
    {
        // 继续加速
        ctrl->state = STEER_STATE_ACCEL;
    }
    else
    {
        // 保持匀速
        ctrl->state = STEER_STATE_CRUISE;
    }
    // 更新速度
    switch (ctrl->state)
    {
        case STEER_STATE_ACCEL:
            ctrl->currentSpeedDegPerMs += ctrl->accelerationDegPerMs2 * (float)inst->config.updateIntervalMs;
            ctrl->currentSpeedDegPerMs = fminf(ctrl->currentSpeedDegPerMs, ctrl->maxSpeedDegPerMs);
            break;

        case STEER_STATE_DECEL:
            ctrl->currentSpeedDegPerMs -= ctrl->accelerationDegPerMs2 * (float)inst->config.updateIntervalMs;
            ctrl->currentSpeedDegPerMs = fmaxf(ctrl->currentSpeedDegPerMs, 0.0f);
            break;

        case STEER_STATE_CRUISE:
            // 速度保持不变
            break;

        default:
            break;
    }
    // 计算本次步长
    const float step = ctrl->currentSpeedDegPerMs * (float)inst->config.updateIntervalMs * direction;
    inst->state.actualAngleDeg += step;

    // 终点判断
    if (remainingDist <= inst->config.deadZoneDeg)
    {
        inst->state.actualAngleDeg = ctrl->targetAngleDeg;
        inst->state.motionState = STEER_STATE_IDLE;
    }

    // 硬件驱动
    const int err = ServoDriver_setAngle(inst->driver, inst->state.actualAngleDeg);
    if (err != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SERVO, "ServoDriver_setAngle failed");
        return err;
    }

    return ERR_SUCCESS;
}

static void handleCalibratingState(SteerInstance* inst)
{
    switch (inst->calibCtrl.calibPhase)
    {
        case 0: // 移动至最大角度
            ServoDriver_setAngle(inst->driver, inst->config.maxAngleDeg);
            if (checkPositionReached(inst))
            {
                inst->calibCtrl.calibPhase++;
            }
            break;

        case 1: // 记录最大值
            inst->config.maxAngleDeg = inst->state.actualAngleDeg;
            inst->calibCtrl.calibPhase++;
            break;

        case 2: // 移动至最小角度
            ServoDriver_setAngle(inst->driver, inst->config.minAngleDeg);
            if (checkPositionReached(inst))
            {
                inst->calibCtrl.calibPhase++;
            }
            break;

        case 3: // 记录最小值
            inst->config.minAngleDeg = inst->state.actualAngleDeg;
            inst->state.motionState = STEER_STATE_IDLE;
            inst->calibCtrl.calibPhase = 0;
            break;
    }
}

/*--------------------- 辅助函数 ---------------------*/
static bool checkPositionReached(const SteerInstance* inst)
{
    // 实际角度与目标角度偏差在死区范围内视为到达
    const float delta = fabsf(inst->state.targetAngleDeg - inst->state.actualAngleDeg);
    return (delta <= inst->config.deadZoneDeg);
}
