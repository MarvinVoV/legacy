/**
 * @file motion_kinematics.c
 * @brief 纯 C 运动学实现（无 STM32 依赖）
 */

#include "motion_kinematics.h"

#include <math.h>
#include <stddef.h>

#define MOTION_PI 3.14159265358979323846f

float motion_clamp_rpm(float rpm, float max_rpm)
{
    if (max_rpm <= 0.0f)
    {
        return 0.0f;
    }
    if (rpm > max_rpm)
    {
        return max_rpm;
    }
    if (rpm < -max_rpm)
    {
        return -max_rpm;
    }
    return rpm;
}

/** 轮子线速度（m/s）→ RPM */
static float wheel_mps_to_rpm(float v_mps, float wheel_radius_m)
{
    if (wheel_radius_m <= 0.0f)
    {
        return 0.0f;
    }
    return v_mps * 60.0f / (2.0f * MOTION_PI * wheel_radius_m);
}

void motion_calc_drive(float linear_mps, float steer_deg, float wheelbase_m,
                       float track_m, float wheel_radius_m, float max_rpm,
                       float* left_rpm, float* right_rpm)
{
    if (left_rpm == NULL || right_rpm == NULL)
    {
        return;
    }

    float v_left  = linear_mps;
    float v_right = linear_mps;

    if (fabsf(steer_deg) >= MOTION_STEER_DEADBAND_DEG && wheelbase_m > 0.0f)
    {
        const float steer_rad = fabsf(steer_deg) * MOTION_PI / 180.0f;
        const float turn_r    = wheelbase_m / tanf(steer_rad);
        const float half_w    = track_m * 0.5f;

        if (steer_deg > 0.0f)
        {
            /* 右转：左轮外侧 */
            v_left  = linear_mps * (turn_r + half_w) / turn_r;
            v_right = linear_mps * (turn_r - half_w) / turn_r;
        }
        else
        {
            /* 左转：右轮外侧 */
            v_left  = linear_mps * (turn_r - half_w) / turn_r;
            v_right = linear_mps * (turn_r + half_w) / turn_r;
        }
    }

    *left_rpm  = motion_clamp_rpm(wheel_mps_to_rpm(v_left, wheel_radius_m), max_rpm);
    *right_rpm = motion_clamp_rpm(wheel_mps_to_rpm(v_right, wheel_radius_m), max_rpm);
}

void motion_calc_differential(float linear_mps, float angular_rps, float track_m,
                              float wheel_radius_m, float max_rpm,
                              float* left_rpm, float* right_rpm)
{
    if (left_rpm == NULL || right_rpm == NULL)
    {
        return;
    }

    const float half_w = track_m * 0.5f;
    const float v_left  = linear_mps - angular_rps * half_w;
    const float v_right = linear_mps + angular_rps * half_w;

    *left_rpm  = motion_clamp_rpm(wheel_mps_to_rpm(v_left, wheel_radius_m), max_rpm);
    *right_rpm = motion_clamp_rpm(wheel_mps_to_rpm(v_right, wheel_radius_m), max_rpm);
}
