/*
 * motion_common.h
 *
 *  Created on: Jul 26, 2025
 *      Author: marvin
 */

#ifndef SYSTEM_COMMON_INC_MOTION_COMMON_H_
#define SYSTEM_COMMON_INC_MOTION_COMMON_H_


typedef enum
{
    MOTION_EMERGENCY_STOP = 0x00, // 紧急停止
    MOTION_DIRECT_CONTROL = 0x01, // 直接控制模式
    MOTION_DIFFERENTIAL   = 0x02, // 纯差速控制（舵机归中）
    MOTION_STEER_PARALLEL = 0x03, // 前轮平行转向模式
    MOTION_SPIN_IN_PLACE  = 0x04, // 原地旋转模式
    MOTION_MIXED_STEER    = 0x05  // 混合转向模式(舵机+差速)
} MotionMode;

typedef enum
{
    DRIVE_CTRL_VELOCITY,  // 速度控制模式
    DRIVE_CTRL_DUTY_CYCLE // 占空比控制模式
} DriveControlType;

#endif /* SYSTEM_COMMON_INC_MOTION_COMMON_H_ */
