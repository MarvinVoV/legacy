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
    MOTION_DIRECT_CONTROL = 0x01, // 直接控制（左右轮 RPM + 舵机角）
    MOTION_DIFFERENTIAL   = 0x02, // 差速控制（舵机归中，linear=0 即原地旋转）
    MOTION_DRIVE          = 0x03, // 速度 + 平行转向（后轮内外速差补偿）
    MOTION_MODE_MAX       = 0x04  // 有效模式数量
} MotionMode;

#endif /* SYSTEM_COMMON_INC_MOTION_COMMON_H_ */
