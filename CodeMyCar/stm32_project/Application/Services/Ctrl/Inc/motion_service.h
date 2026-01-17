/*
 * motion_service.h
 *
 *  Created on: Apr 6, 2025
 *      Author: marvin
 */

#ifndef SERVICES_CTRL_INC_MOTION_SERVICE_H_
#define SERVICES_CTRL_INC_MOTION_SERVICE_H_
#include "motion_common.h"
#include "motor_service.h"
#include "servo_service.h"

//------------------------------------------------------------------------------
// 类型定义
//------------------------------------------------------------------------------

/**
 * @brief 底盘物理配置参数
 */
typedef struct
{
    float trackWidthMM;         ///< 轮距 (mm)
    float wheelbaseMM;          ///< 轴距 (mm)
    float wheelRadiusMM;        ///< 车轮半径 (mm)
    float minTurnRadiusMM;      ///< 最小转弯半径 (mm)
    float maxSteerAngleDeg;     ///< 最大转向角度 (deg)
    float maxAngularVelRad;     ///< 最大角速度 (rad/s)
    float maxLinearVelocityMPS; ///< 最大线速度 (m/s)
    float steerCenterAngleDeg;  ///< 中心角度 (deg)
} ChassisConfig;


/**
 * @brief 运动控制目标参数
 */
typedef struct
{
    MotionMode mode; ///< 当前控制模式

    union
    {
        // 差度控制参数
        struct
        {
            float linearVelocity;  ///< 目标线速度 (m/s)
            float angularVelocity; ///< 目标角速度 (rad/s)
        } diffCtrl;

        // 前轮平行转向参数
        struct
        {
            float linearVelocity; ///< 目标线速度 (m/s)
            float steerAngle;     ///< 目标转向角 (deg)
        } parallelSteer;

        // 直接控制参数
        struct
        {
            float leftRpm;    ///< 左轮目标转速 (RPM)
            float rightRpm;   ///< 右轮目标转速 (RPM)
            float steerAngle; ///< 目标转向角 (deg)
        } directCtrl;

        struct
        {
            float   angularVelocity; ///< 角速度 (0.0644 rad/s步长)
            uint8_t rotationPoint;   ///< 旋转中心点 (0=中心, 1=前轴, 2=后轴)
        } spinCtrl;

        // 混合控制参数
        struct
        {
            DriveControlType driveCtrlType; ///< 驱动控制类型
            union
            {
                struct
                {
                    float linearVelocity;  ///< 目标线速度 (m/s)
                    float angularVelocity; ///< 目标角速度 (rad/s)
                } velocityCtrl;

                struct
                {
                    float leftDutyCycle;  ///< 左轮占空比 (-1.0~1.0)
                    float rightDutyCycle; ///< 右轮占空比 (-1.0~1.0)
                } dutyCycleCtrl;
            } driveParams;

            float steerAngle;    ///< 目标转向角 (deg)
            float steeringBlend; ///< 转向混合比例 (0.0~1.0)
        } mixedCtrl;
    } params;
} MotionTarget;

/**
 * @brief 运动系统实时状态
 */
typedef struct
{
    MotionMode currentMode; ///< 当前控制模式
    uint32_t   timestamp;   ///< 状态更新时间戳 (ms)

    struct
    {
        float leftRpm;  ///< 左轮实际转速 (RPM)
        float rightRpm; ///< 右轮实际转速 (RPM)
    } drive;

    struct
    {
        float actualAngle; ///< 实际转向角 (deg)
        float targetAngle; ///< 目标转向角 (deg)
    } steering;

    struct
    {
        float linear;       ///< 整车线速度 (m/s)
        float angular;      ///< 整车角速度 (rad/s)
        float acceleration; ///< 当前加速度 (m/s²)
    } velocity;

    struct
    {
        float x;             ///< X轴坐标 (m)
        float y;             ///< Y轴坐标 (m)
        float theta;         ///< 航向角 (rad)
        float distance;      ///< 累计行驶距离 (m)
        float leftOdometer;  ///< 左轮累计行程 (m)
        float rightOdometer; ///< 右轮累计行程 (m)
    } pose;
} MotionState;

/**
 * @brief 运动控制上下文
 */
typedef struct
{
    MotorService*  motorService;  ///< 电机控制服务
    SteerInstance* steerServo;    ///< 转向舵机服务
    ChassisConfig  chassisConfig; ///< 底盘物理配置
    MotionTarget   motionTarget;  ///< 运动控制目标
    MotionState    state;         ///< 运动系统状态
    osMutexId_t    targetMutex;   ///< 目标参数互斥锁
    float          lastLeftRevs;  ///< 左轮上次转数
    float          lastRightRevs; ///< 右轮上次转数
} MotionContext;


//------------------------------------------------------------------------------
// 公共API
//------------------------------------------------------------------------------

/**
 * @brief 初始化运动控制上下文
 *
 * @param ctx 上下文指针
 * @param motor 电机服务实例
 * @param steer 转向服务实例
 * @param config 底盘配置参数
 *
 * @retval ERR_SUCCESS 初始化成功
 */
int MotionContext_Init(MotionContext* ctx, MotorService* motor, SteerInstance* steer, const ChassisConfig* config);

/**
 * @brief 设置运动控制模式
 *
 * @param ctx 上下文指针
 * @param new_mode 目标控制模式
 *
 * @retval ERR_SUCCESS 设置成功
 * @retval ERR_MOTION_INVALID_MODE 无效模式
 */
int MotionService_SetControlMode(MotionContext* ctx, MotionMode new_mode);

/**
 * @brief 获取当前运动控制模式
 *
 * @param ctx 运动控制上下文指针
 * @return MotionMode 当前控制模式
 */
MotionMode MotionService_GetCurrentMode(const MotionContext* ctx);


/**
 * @brief 设置差速控制参数
 *
 * @param ctx 运动控制上下文
 * @param linear 目标线速度 (m/s)
 * @param angular 目标角速度 (rad/s)
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTION_INVALID_PARAM 无效参数
 * @retval ERR_MOTION_MODE_MISMATCH 当前非差速模式
 * @retval ERR_MOTION_MUTEX_TIMEOUT 互斥锁超时
 */
int MotionService_SetDifferentialParams(MotionContext* ctx, float linear, float angular);

/**
 * @brief 设置直接控制参数
 *
 * @param ctx 上下文指针
 * @param left_rpm 左轮目标转速 (RPM)
 * @param right_rpm 右轮目标转速 (RPM)
 * @param steer_angle 目标转向角 (deg)
 *
 * @retval ERR_SUCCESS 设置成功
 * @retval ERR_MOTION_INVALID_PARAM 参数无效
 * @retval ERR_MOTION_MODE_MISMATCH 模式不匹配
 */
int MotionService_SetDirectControlParams(MotionContext* ctx, float left_rpm, float right_rpm, float steer_angle);

/**
 * @brief 设置前轮平行转向参数 (MOTION_STEER_PARALLEL/MIXED模式)
 *
 * @param ctx 上下文指针
 * @param linear_vel 目标线速度 (m/s)
 * @param steer_angle 目标转向角 (deg)
 *
 * @retval ERR_SUCCESS0 设置成功
 * @retval ERR_MOTION_INVALID_PARAM 参数无效
 * @retval ERR_MOTION_MODE_MISMATCH 模式不匹配
 */
int MotionService_SetSteerParallelParams(MotionContext* ctx, float linear_vel, float steer_angle);

/**
 * @brief 设置混合控制参数（速度控制模式）
 *
 * @param ctx 运动控制上下文
 * @param linear 目标线速度 (m/s)
 * @param angular 目标角速度 (rad/s)
 * @param steer_angle 目标转向角 (deg)
 * @param steering_blend 转向混合比例 (0.0~1.0)
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTION_INVALID_PARAM 无效参数
 */
int MotionService_SetMixedVelocityParams(MotionContext* ctx,
                                         float          linear,
                                         float          angular,
                                         float          steer_angle,
                                         float          steering_blend);

/**
 * @brief 设置混合控制参数（占空比控制模式）
 *
 * @param ctx 运动控制上下文
 * @param left_duty 左轮占空比 (-1.0~1.0)
 * @param right_duty 右轮占空比 (-1.0~1.0)
 * @param steer_angle 目标转向角 (deg)
 * @param steering_blend 转向混合比例 (0.0~1.0)
 *
 * @retval ERR_SUCCESS 操作成功
 * @retval ERR_MOTION_INVALID_PARAM 无效参数
 */
int MotionService_SetMixedDutyParams(MotionContext* ctx,
                                     float          left_duty,
                                     float          right_duty,
                                     float          steer_angle,
                                     float          steering_blend);
/**
 * @brief 设置原地旋转参数
 *
 * @param ctx 上下文指针
 * @param angular_vel 目标角速度 (rad/s)
 * @param rotation_point 旋转点 0=中心, 1=前轴, 2=后轴
 *
 * @retval ERR_SUCCESS0 设置成功
 * @retval ERR_MOTION_INVALID_PARAM 参数无效
 * @retval ERR_MOTION_MODE_MISMATCH 模式不匹配
 */
int MotionService_SetSpinParams(MotionContext* ctx, float angular_vel, uint8_t rotation_point);

/**
 * @brief 设置转向角度 （STEER_ONLY/MIXED模式）
 *
 * @param ctx 上下文指针
 * @param angle_deg 转向角度 (deg)
 *
 * @retval ERR_SUCCESS0 设置成功
 * @retval ERR_MOTION_INVALID_PARAM 参数无效
 * @retval ERR_MOTION_MODE_MISMATCH 模式不匹配
 */
int MotionService_SetSteerAngle(MotionContext* ctx, float angle_deg);


/**
 * @brief 执行紧急停止
 *
 * @param ctx 上下文指针
 *
 * @retval ERR_SUCCESS0 设置成功
 * @retval ERR_MOTION_INVALID_PARAM 参数无效
 */
int MotionService_EmergencyStop(MotionContext* ctx);


/**
 * @brief 获取运动状态
 *
 * @param ctx 上下文指针
 * @param out_state 输出状态指针
 */
void MotionService_GetState(const MotionContext* ctx, MotionState* out_state);

/**
 * @brief 运行运动控制服务
 *
 * @param ctx 上下文指针
 *
 * @note 应在控制循环中周期性调用
 */
void MotionService_Run(MotionContext* ctx);


#endif /* SERVICES_CTRL_INC_MOTION_SERVICE_H_ */
