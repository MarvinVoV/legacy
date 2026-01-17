/*
 * ctrl_protocol.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef SYSTEM_PROTOCOL_INC_CTRL_PROTOCOL_H_
#define SYSTEM_PROTOCOL_INC_CTRL_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>
#include "motion_common.h"

#pragma pack(push, 1)


//------------------------------------------------------------------------------
// 类型定义
//------------------------------------------------------------------------------
/**
 * @brief 控制字段掩码
 */
typedef enum
{
    CTRL_FIELD_MOTION = 0x01, ///< 运动控制字段
    CTRL_FIELD_SENSOR = 0x02, ///< 传感器控制字段
    CTRL_FIELD_ALL    = 0x0F  ///< 所有控制字段
} CtrlField;


/**
 * @brief 差速控制参数
 */
typedef struct
{
    int16_t linearVel;  ///< 线速度 (0.001 m/s步长)
    int16_t angularVel; ///< 角速度 (0.0644 rad/s步长)
} DiffCtrlParam;

/**
 * @brief 直接控制参数
 */
typedef struct
{
    int16_t leftRpm;    ///< 左轮目标转速 (RPM)
    int16_t rightRpm;   ///< 右轮目标转速 (RPM)
    int16_t steerAngle; ///< 转向角 (0.1度步长)
} DirectCtrlParam;

/**
 * @brief 平行转向参数
 */
typedef struct
{
    int16_t linearVel;  ///< 线速度 (0.001 m/s步长)
    int16_t steerAngle; ///< 转向角 (0.1度步长)
} SteerParallelParam;

/**
 * @brief 混合控制参数
 */
typedef struct
{
    DriveControlType driveCtrlType; ///< 驱动控制类型

    union
    {
        // 速度控制参数
        struct
        {
            /**
             * @brief 目标线速度
             *
             * 单位：0.001 m/s步长
             * 范围：-32768 ~ +32767
             * 实际速度 = linearVel * 0.001 (m/s)
             * 示例：
             *  1000 = 1.0 m/s
             *  2000 = 2.0 m/s
             *  -1500 = -1.5 m/s
             */
            int16_t linearVel; ///< 目标线速度 (0.001 m/s步长)
            /**
             * @brief 目标角速度
             *
             * 单位：0.0644 rad/s步长
             * 范围：-32768 ~ +32767
             * 实际角速度 = angularVel * 0.0644 (rad/s)
             * 示例：
             *  15 = 0.966 rad/s (约55.3°/s)
             *  31 = 2.0 rad/s (约114.6°/s)
             *  -15 = -0.966 rad/s
             */
            int16_t angularVel; ///< 目标角速度 (0.0644 rad/s步长)
        } velocityCtrl;

        // 占空比控制参数
        struct
        {
            int16_t leftDutyCycle;  ///< 左轮占空比 (-1000~1000 表示 -100.0% ~ 100.0%)
            int16_t rightDutyCycle; ///< 右轮占空比 (-1000~1000 表示 -100.0% ~ 100.0%)
        } dutyCycleCtrl;
    } driveParams;

    int16_t steerAngle;       ///< 转向角 (0.1度步长)
    uint8_t differentialGain; ///< 差速增益 (0-100%)
} MixedCtrlParam;


/**
 * @brief 原地旋转控制参数
 */
typedef struct
{
    /**
     * @brief 目标角速度
     *
     * 分辨率：0.0644 rad/s
     * 角速度范围: 0 ~ 12rad/s （按照最大转速 300rpm 设置）
     * 解码前设置范围 ：-186 ~ 186, angular_vel = 12 / 0.0644 ≈ 186.34
     */
    int16_t angularVel;    ///< 角速度 (0.0644 rad/s步长)
    uint8_t rotationPoint; ///< 旋转中心点 (0=中心, 1=前轴, 2=后轴)
} SpinCtrlParam;

/**
 * @brief 运动控制指令
 */
typedef struct
{
    MotionMode mode;     ///< 运动模式
    uint16_t   duration; ///< 执行持续时间 (ms)
    union
    {
        SteerParallelParam steerParallelCtrl; ///< 平行转向参数
        DirectCtrlParam    directCtrl;        ///< 直接控制参数
        MixedCtrlParam     mixedCtrl;         ///< 混合控制参数
        DiffCtrlParam      diffCtrl;          ///< 差速控制参数
        SpinCtrlParam      spinCtrl;          ///< 旋转参数
    } params;
} MotionCmd;


/**
 * @brief 主控制指令
 */
typedef struct
{
    uint8_t   ctrlId; ///< 控制器ID (0-15)
    uint8_t   fields; ///< 控制字段掩码
    MotionCmd motion; ///< 运动控制指令
} ControlCmd;

#pragma pack(pop)


/**
 * @brief 检查是否包含指定控制字段
 * @param fields 当前控制字段掩码
 * @param field 要检查的字段掩码
 * @return 包含返回true，否则false
 */
bool isCtrlFieldSet(uint8_t fields, CtrlField field);

/**
 * @brief 检查是否同时包含多个控制字段
 * @param fields 当前控制字段掩码
 * @param checkFields 要检查的字段掩码组合
 * @return 全部包含返回true，否则false
 */
bool isCtrlFieldsSet(uint8_t fields, uint8_t checkFields);


/**
 * @brief 验证控制指令完整性
 *
 * @param cmd 控制指令指针
 * @param len 数据长度
 */
int validateControlCmd(const uint8_t* cmd, uint16_t len);


#endif /* SYSTEM_PROTOCOL_INC_CTRL_PROTOCOL_H_ */
