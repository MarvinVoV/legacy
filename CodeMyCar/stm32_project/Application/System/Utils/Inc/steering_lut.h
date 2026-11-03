#ifndef STEERING_LUT_H
#define STEERING_LUT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 转向标定表（2026-11-03 实测）
 *
 * 上层控制量是曲率 kappa (1/m)，右转为正；本模块负责
 *   kappa -> 等效前轮转角 delta -> 舵机脉宽 -> 舵机原始偏移
 *
 * 数据来源：Arduino UNO R4 在 A0 上直接发 50Hz PWM，逐点测量左右前轮角度。
 *   1500us = 前轮正中（直行），+38.5° = 950us，-28.5° = 2250us。
 * 左右不对称（舵机转轴偏左）与两端饱和全部由表吸收，代码中不再有左右系数
 * 或中位补偿。
 */

/** 曲率上限 (1/m)：由弱侧（左转 28.5°）决定，左右对称使用。 */
#define STEERING_KAPPA_MAX_1PM (3.770f)

/** 等效前轮转角上限（度），与 KAPPA_MAX 对应。 */
#define STEERING_ANGLE_MAX_DEG (28.5f)

/** 舵机名义中位脉宽 (us)，对应前轮正中。 */
#define STEERING_SERVO_CENTER_US (1500.0f)

/** 舵机脉宽与角度的换算：500..2500us <-> 0..180°。 */
#define STEERING_SERVO_US_PER_DEG (2000.0f / 180.0f)

/** 等效前轮转角 -> 舵机脉宽。角度超出标定表范围返回 false。 */
bool steering_lut_angle_to_pulse(float angle_deg, float* pulse_us);

/** 曲率 -> 等效前轮转角。`|kappa| > STEERING_KAPPA_MAX_1PM` 返回 false。 */
bool steering_lut_curvature_to_angle(float kappa_1pm, float wheelbase_m, float* angle_deg);

/** 等效前轮转角 -> 曲率。`|angle| > STEERING_ANGLE_MAX_DEG` 返回 false。 */
bool steering_lut_angle_to_curvature(float angle_deg, float wheelbase_m, float* kappa_1pm);

/** 曲率 -> 舵机脉宽（组合上面两步）。 */
bool steering_lut_curvature_to_pulse(float kappa_1pm, float wheelbase_m, float* pulse_us);

/** 曲率 -> 舵机原始偏移角（正 = 右；相对名义中位）。 */
bool steering_lut_curvature_to_raw(float kappa_1pm, float wheelbase_m, float* raw_deg);

/** 标定点数量（供自检/标定工具使用）。 */
size_t steering_lut_point_count(void);

/** 读取第 index 个标定点；越界返回 false。 */
bool steering_lut_get_point(size_t index, float* angle_deg, float* pulse_us);

#endif
