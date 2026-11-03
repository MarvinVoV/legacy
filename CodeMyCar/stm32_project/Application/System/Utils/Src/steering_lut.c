#include "steering_lut.h"

#include <math.h>
#include <stddef.h>

#define STEER_RAD2DEG (57.29577951308232f)
#define STEER_DEG2RAD (0.017453292519943295f)

/*
 * 临时机械补偿（2026-11-03 撞车后，2026-11-04 地面实测标定）
 *
 * 舵机臂跳齿：1500us 已不是前轮正中。地面直行测试（DIRECT 29/29 RPM、
 * 0.67 m）实测漂移：
 *     1500us -> -8.4 deg/m
 *     1533us -> -2.6 deg/m
 *     1540us -> +0.1 deg/m   <- 真正走直
 *     1600us -> +16.8 deg/m
 * 因此整条脉宽曲线右移 +40us，使 kappa = 0 输出 1540us；输出夹在实测
 * 安全范围 [950, 2250]us，避免左满顶死（原左端只有 2–3 mm 余量）。
 *
 * 机械复位（拆舵机臂重新对中，让 1500us 重新等于前轮正中）后，必须把
 * STEER_PULSE_OFFSET_US 改回 0 并重跑地面直行标定。
 */
#define STEER_PULSE_OFFSET_US (40.0f)
#define STEER_PULSE_SAFE_MIN_US (950.0f)
#define STEER_PULSE_SAFE_MAX_US (2250.0f)

/*
 * 2026-11-03 实测标定（Arduino UNO R4 在 A0 上直接发 50Hz PWM）：
 *
 *   脉宽 1500us = 前轮正中（直行）；右转 = 脉宽减小，左转 = 脉宽增大。
 *   角度为左右前轮实测转角的平均值，右转为正。
 *
 * 表按角度升序、脉宽降序排列；两端为标定测量点，不是机械极限
 * （机械极限见 Docs/Steering-Control-Design.md 第 2 节）。
 *
 * 版本：v1 (2026-11-03)
 */
typedef struct
{
    float angleDeg;
    float pulseUs;
} SteerLutPoint;

static const SteerLutPoint kSteerLut[] = {
    { -28.5f, 2250.0f }, { -27.0f, 2150.0f }, { -24.0f, 2050.0f },
    { -19.5f, 1950.0f }, { -16.0f, 1850.0f }, { -11.5f, 1750.0f },
    { -8.0f, 1650.0f },  { 0.0f, 1500.0f },   { 10.0f, 1350.0f },
    { 13.5f, 1250.0f },  { 24.5f, 1150.0f },  { 33.0f, 1030.0f },
    { 38.5f, 950.0f },
};

#define STEER_LUT_COUNT (sizeof(kSteerLut) / sizeof(kSteerLut[0]))

static bool lut_lookup(float angle_deg, float* pulse_us)
{
    if (pulse_us == NULL || !isfinite(angle_deg))
    {
        return false;
    }
    if (angle_deg < kSteerLut[0].angleDeg ||
        angle_deg > kSteerLut[STEER_LUT_COUNT - 1].angleDeg)
    {
        return false;
    }

    for (size_t i = 0; i + 1 < STEER_LUT_COUNT; ++i)
    {
        const float a0 = kSteerLut[i].angleDeg;
        const float a1 = kSteerLut[i + 1].angleDeg;
        if (angle_deg >= a0 && angle_deg <= a1)
        {
            const float t = (a1 > a0) ? (angle_deg - a0) / (a1 - a0) : 0.0f;
            *pulse_us = kSteerLut[i].pulseUs +
                        t * (kSteerLut[i + 1].pulseUs - kSteerLut[i].pulseUs);
            return true;
        }
    }
    return false;
}

bool steering_lut_angle_to_pulse(float angle_deg, float* pulse_us)
{
    return lut_lookup(angle_deg, pulse_us);
}

bool steering_lut_curvature_to_angle(float kappa_1pm, float wheelbase_m, float* angle_deg)
{
    if (angle_deg == NULL || !isfinite(kappa_1pm) || !isfinite(wheelbase_m) ||
        wheelbase_m <= 0.0f)
    {
        return false;
    }
    if (kappa_1pm > STEERING_KAPPA_MAX_1PM || kappa_1pm < -STEERING_KAPPA_MAX_1PM)
    {
        return false;
    }
    *angle_deg = atanf(wheelbase_m * kappa_1pm) * STEER_RAD2DEG;
    return true;
}

bool steering_lut_angle_to_curvature(float angle_deg, float wheelbase_m, float* kappa_1pm)
{
    if (kappa_1pm == NULL || !isfinite(angle_deg) || !isfinite(wheelbase_m) ||
        wheelbase_m <= 0.0f)
    {
        return false;
    }
    if (angle_deg > STEERING_ANGLE_MAX_DEG || angle_deg < -STEERING_ANGLE_MAX_DEG)
    {
        return false;
    }
    *kappa_1pm = tanf(angle_deg * STEER_DEG2RAD) / wheelbase_m;
    return true;
}

bool steering_lut_curvature_to_pulse(float kappa_1pm, float wheelbase_m, float* pulse_us)
{
    float angle_deg = 0.0f;
    float pulse = 0.0f;
    if (pulse_us == NULL ||
        !steering_lut_curvature_to_angle(kappa_1pm, wheelbase_m, &angle_deg) ||
        !lut_lookup(angle_deg, &pulse))
    {
        return false;
    }

    pulse += STEER_PULSE_OFFSET_US;
    if (pulse < STEER_PULSE_SAFE_MIN_US) pulse = STEER_PULSE_SAFE_MIN_US;
    if (pulse > STEER_PULSE_SAFE_MAX_US) pulse = STEER_PULSE_SAFE_MAX_US;
    *pulse_us = pulse;
    return true;
}

bool steering_lut_curvature_to_raw(float kappa_1pm, float wheelbase_m, float* raw_deg)
{
    float pulse_us = 0.0f;
    if (raw_deg == NULL ||
        !steering_lut_curvature_to_pulse(kappa_1pm, wheelbase_m, &pulse_us))
    {
        return false;
    }
    *raw_deg = (STEERING_SERVO_CENTER_US - pulse_us) / STEERING_SERVO_US_PER_DEG;
    return true;
}

size_t steering_lut_point_count(void)
{
    return STEER_LUT_COUNT;
}

bool steering_lut_get_point(size_t index, float* angle_deg, float* pulse_us)
{
    if (angle_deg == NULL || pulse_us == NULL || index >= STEER_LUT_COUNT)
    {
        return false;
    }
    *angle_deg = kSteerLut[index].angleDeg;
    *pulse_us = kSteerLut[index].pulseUs;
    return true;
}
