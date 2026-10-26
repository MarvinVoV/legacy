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
 * @brief 驱动参数（DRIVE：速度 + 平行转向角）
 */
typedef struct
{
    int16_t linearVel;  ///< 线速度 (0.001 m/s步长, -1000..1000)
    int16_t steerAngle; ///< 转向角 (0.1度步长, -450..450)
} DriveCtrlParam;

/**
 * @brief 目标型运动参数（MOVE：相对距离 + 相对转角）
 *
 * turnDdeg 为相对增量，右转为正，不做归一化（270° 存 2700）。
 */
typedef struct
{
    int16_t  turnDdeg;   ///< 航向变化量 (0.1度, 右正, ±3600)
    int16_t  distanceMm; ///< 路程 (mm, 前进正, ±5000)
    uint16_t speedMmps;  ///< 路径速度 (mm/s, 1..600)
} GoalCtrlParam;

/**
 * @brief 运动控制指令
 */
typedef struct
{
    uint8_t mode; ///< 运动模式（MotionMode，显式宽度以钉死 wire 布局）
    union
    {
        DriveCtrlParam  driveCtrl;  ///< DRIVE
        DirectCtrlParam directCtrl; ///< DIRECT
        DiffCtrlParam   diffCtrl;   ///< DIFFERENTIAL
        GoalCtrlParam   goalCtrl;   ///< MOTION_GOAL
    } params;
} MotionCmd;


/**
 * @brief 主控制指令（9 字节）
 */
typedef struct
{
    uint8_t   ctrlId; ///< 控制器ID (0-15)
    uint8_t   fields; ///< 控制字段掩码
    MotionCmd motion; ///< 运动控制指令
} ControlCmd;

#pragma pack(pop)

/** wire payload 固定大小：ctrlId(1)+fields(1)+mode(1)+params(6) */
#define CONTROL_CMD_SIZE 9

_Static_assert(sizeof(ControlCmd) == CONTROL_CMD_SIZE,
               "ControlCmd wire layout must stay 9 bytes");


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
