#pragma once

/**
 * @file motion_kinematics.h
 * @brief 纯 C 运动学计算（可 host 测试，不依赖 STM32 HAL）
 *
 * 仅包含：DRIVE（速度+转角 → 左右轮速）、DIFFERENTIAL（速度+角速度 → 左右轮速）、
 * RPM 限幅。硬件控制/舵机/电机由 motion_service 负责。
 */

#include <stdint.h>

/** 驱动轮最大转速（与 MotorSpec.maxRPM 一致） */
#define MOTION_MAX_RPM 300.0f

/** 转向角小于该值（度）视为直行，避免 tan(0)/极小转角导致数值放大 */
#define MOTION_STEER_DEADBAND_DEG 0.1f

/**
 * @brief 把目标 RPM 限幅到 ±max_rpm
 */
float motion_clamp_rpm(float rpm, float max_rpm);

/**
 * @brief DRIVE 模式：线速度 + 转向角 → 左右轮目标 RPM
 *
 * 舵机为单舵机平行转向；这里对后轮做内外速差补偿：
 *   R = wheelbase / tan(|δ|)
 *   δ>0（右转）：左轮为外侧，速度 = v*(R+W/2)/R
 *   δ<0（左转）：右轮为外侧，速度 = v*(R+W/2)/R
 *
 * @param linear_mps    目标线速度 (m/s)，负值倒车
 * @param steer_deg     转向角 (deg)，>0 右转
 * @param wheelbase_m   轴距 (m)
 * @param track_m       轮距 (m)
 * @param wheel_radius_m 轮半径 (m)
 * @param max_rpm       轮速上限
 * @param left_rpm      [out] 左轮目标 RPM
 * @param right_rpm     [out] 右轮目标 RPM
 */
void motion_calc_drive(float linear_mps, float steer_deg, float wheelbase_m,
                       float track_m, float wheel_radius_m, float max_rpm,
                       float* left_rpm, float* right_rpm);

/**
 * @brief DIFFERENTIAL 模式：线速度 + 角速度 → 左右轮目标 RPM
 *
 * left  = linear - angular*W/2
 * right = linear + angular*W/2
 * （angular>0 时右轮更快，即左转/逆时针；linear=0 为原地旋转）
 */
void motion_calc_differential(float linear_mps, float angular_rps, float track_m,
                              float wheel_radius_m, float max_rpm,
                              float* left_rpm, float* right_rpm);
