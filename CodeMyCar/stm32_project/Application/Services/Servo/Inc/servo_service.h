/*
 * servo_service.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef SERVICES_SERVO_INC_SERVO_SERVICE_H_
#define SERVICES_SERVO_INC_SERVO_SERVICE_H_

#include <macros.h>
#include "cmsis_os2.h"
#include "ctrl_protocol.h"
#include "servo_driver.h"

// 默认移动速度 200ms/度
#define SERVO_DEFAULT_SPEED_MS (200)

/*--------------------- 配置参数 ---------------------*/
typedef struct
{
    float minAngleDeg;            // 机械最小角度（度）
    float maxAngleDeg;            // 机械最大角度（度）
    float deadZoneDeg;            // 死区角度（度）
    float defaultStepDeg;         // 默认单步角度（度）
    float maxSpeedDegPerSec;      // 最大速度：90°/秒（即 0.025°/ms）
    float accelerationDegPerSec2; // 加速度：180°/秒²（即 0.18°/ms²）
    uint16_t updateIntervalMs;    // 控制周期（毫秒）
} SteerConfig;

/*--------------------- 运行时状态 ---------------------*/
// 运动状态枚举
typedef enum
{
    STEER_MOTION_STATE_IDLE = 0x01, // 空闲状态
    STEER_MOTION_STATE_MOVING,      // 运动中
    STEER_MOTION_STATE_ERROR,       // 错误状态
    STEER_MOTION_STATE_CALIBRATING  // 校准中
} SteerMotionState;

typedef struct
{
    float actualAngleDeg;    // 实际角度（度）
    float targetAngleDeg;    // 目标角度（度）
    uint32_t lastUpdateTick; // 最后更新时间戳
    uint16_t motionState;    // 运动状态枚举值
    uint32_t errorCode;      // 错误码
} SteerState;

typedef struct
{
    float targetAngleDeg;        // 目标角度
    float currentSpeedDegPerMs;  // 当前速度（度/毫秒）
    float accelerationDegPerMs2; // 加速度（度/毫秒²）
    float maxSpeedDegPerMs;      // 最大速度（度/毫秒）
    enum {
        STEER_STATE_ACCEL,
        STEER_STATE_CRUISE,
        STEER_STATE_DECEL,
        STEER_STATE_IDLE
    } state;
} SmoothControlContext;

typedef struct
{
    // 硬件层依赖
    ServoDriver* driver;

    // 配置参数（初始化后只读）
    SteerConfig config;

    // 运行时状态
    SteerState state;

    // 平滑控制上下文
    SmoothControlContext smoothCtrl;

    struct
    {
        uint8_t calibPhase;
        float calibStartAngle;
    } calibCtrl; // 校准状态

    osMutexId_t mutex;
} SteerInstance;

/**
 * @brief 初始化舵机控制服务实例
 * @param steerInstance 舵机实例指针
 * @param config 舵机配置参数（机械参数、控制参数等），必须非空
 * @param driver 硬件驱动接口实现，必须非空
 * @return SteerInstance* 成功返回实例指针，失败返回NULL
 * @note 此函数会动态分配内存，使用后需调用 SteerService_Deinit 释放资源
 */
int SteerService_init(SteerInstance* steerInstance, SteerConfig* config, ServoDriver* driver);


/**
 * @brief 平滑设置舵机角度（亚角度级精度）
 * @param instance 舵机实例指针
 * @param angle 目标角度（单位：度，支持小数精度）
 * @return int 错误码：
 *   - ERR_SUCCESS 操作成功
 *   - ERR_INVALID_PARAM 参数无效
 *   - ERR_ANGLE_OUTOFRANGE 目标角度超出机械限制
 * @note 实际运动时间 = fabs(目标角度 - 当前角度) * speedMs
 */
int SteerService_setAngleSmoothly(SteerInstance* instance, float angle);

/**
 * @brief 立即设置舵机角度（无过渡）
 * @param instance 舵机实例指针
 * @param angle 目标角度（单位：度）
 * @return int 错误码：
 *   - ERR_SUCCESS 操作成功
 *   - ERR_INVALID_PARAM 参数无效
 * @warning 此操作会中断正在进行的平滑运动，直接更新硬件位置
 */
int SteerService_setAngleImmediate(SteerInstance* instance, float angle);

/**
 * @brief 更新舵机控制状态（需周期性调用）
 * @param instance 舵机实例指针
 * @return int 错误码：
 *   - ERR_SUCCESS 状态更新成功
 *   - ERR_STEER_TOO_FREQUENT 调用过于频繁
 *   - ERR_HARDWARE_FAILURE 硬件通信失败
 * @note 建议调用频率与 config.updateIntervalMs 设置一致
 */
int SteerService_update(SteerInstance* instance);

/**
 * @brief 紧急停止舵机运动
 * @param inst 舵机实例指针
 * @note 此操作会：
 *   1. 立即停止所有运动
 *   2. 复位平滑控制上下文
 *   3. 调用驱动层紧急停止函数
 *   4. 更新状态为 STEER_STATE_IDLE
 */
void SteerService_forceStop(SteerInstance* inst);

/**
 * @brief 获取当前舵机状态（线程安全）
 * @param inst 舵机实例指针
 * @return SteerState 状态快照副本
 * @warning 返回的结构体是临时拷贝，如需长期使用需自行保存数据
 */
SteerState SteerService_getState(const SteerInstance* inst);


#endif /* SERVICES_SERVO_INC_SERVO_SERVICE_H_ */
