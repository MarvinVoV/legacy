#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "steering_lut.h"

#define L_M (0.144f)

static void near(float actual, float expected, float tol)
{
    assert(fabsf(actual - expected) < tol);
}

int main(void)
{
    float out = 0.0f;

    /* 2026-11-03 实测标定：1500us = 直行，+38.5° = 950us，-28.5° = 2250us */

    /* ---- 角度 → 脉宽：标定点精确命中 ---- */
    assert(steering_lut_angle_to_pulse(0.0f, &out));   near(out, 1500.0f, 0.01f);
    assert(steering_lut_angle_to_pulse(38.5f, &out));  near(out, 950.0f, 0.01f);
    assert(steering_lut_angle_to_pulse(-28.5f, &out)); near(out, 2250.0f, 0.01f);
    assert(steering_lut_angle_to_pulse(10.0f, &out));  near(out, 1350.0f, 0.01f);
    assert(steering_lut_angle_to_pulse(-16.0f, &out)); near(out, 1850.0f, 0.01f);

    /* ---- 分段线性插值 ---- */
    assert(steering_lut_angle_to_pulse(28.5f, &out));   near(out, 1093.53f, 0.1f);
    assert(steering_lut_angle_to_pulse(20.0f, &out));   near(out, 1190.91f, 0.1f);
    assert(steering_lut_angle_to_pulse(-14.25f, &out)); near(out, 1811.11f, 0.1f);

    /* ---- 角度越界/非法输入拒绝 ---- */
    assert(!steering_lut_angle_to_pulse(38.6f, &out));
    assert(!steering_lut_angle_to_pulse(-28.6f, &out));
    assert(!steering_lut_angle_to_pulse(NAN, &out));
    assert(!steering_lut_angle_to_pulse(0.0f, NULL));

    /* ---- 曲率 → 角度 ---- */
    assert(steering_lut_curvature_to_angle(0.0f, L_M, &out)); near(out, 0.0f, 0.001f);
    assert(steering_lut_curvature_to_angle(STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, 28.5f, 0.01f);
    assert(steering_lut_curvature_to_angle(-STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, -28.5f, 0.01f);

    /* 曲率超出对称上限（弱侧 28.5°）必须拒绝 */
    assert(!steering_lut_curvature_to_angle(STEERING_KAPPA_MAX_1PM + 0.01f, L_M, &out));
    assert(!steering_lut_curvature_to_angle(NAN, L_M, &out));
    assert(!steering_lut_curvature_to_angle(0.0f, 0.0f, &out));

    /* ---- 曲率 ↔ 角度 往返 ---- */
    float kappa = 0.0f;
    assert(steering_lut_curvature_to_angle(1.5f, L_M, &out));
    assert(steering_lut_angle_to_curvature(out, L_M, &kappa));
    near(kappa, 1.5f, 1e-4f);
    assert(!steering_lut_angle_to_curvature(30.0f, L_M, &kappa));

    /* ---- 曲率 → 脉宽（端到端） ----
       当前装配中位1530us与启动安全脉冲共用，
       输出仍夹在 [950, 2250]us 安全范围内。 */
    assert(steering_lut_curvature_to_pulse(0.0f, L_M, &out)); near(out, STEERING_SERVO_STRAIGHT_US, 0.5f);
    assert(steering_lut_curvature_to_pulse(STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, 1123.58f, 0.5f);
    assert(steering_lut_curvature_to_pulse(-STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, 2250.0f, 0.5f);

    /* ---- 曲率 → 舵机原始偏移（正 = 右，raw = (1500 - pulse) × 0.09） ---- */
    assert(steering_lut_curvature_to_raw(0.0f, L_M, &out)); near(out, -2.70f, 0.05f);
    assert(steering_lut_curvature_to_raw(-STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, -67.5f, 0.05f);
    assert(steering_lut_curvature_to_raw(STEERING_KAPPA_MAX_1PM, L_M, &out));
    near(out, 33.88f, 0.1f);

    /* ---- 表结构：角度严格递增、脉宽严格递减 ---- */
    const size_t n = steering_lut_point_count();
    assert(n == 13);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        float a0 = 0.0f, p0 = 0.0f, a1 = 0.0f, p1 = 0.0f;
        assert(steering_lut_get_point(i, &a0, &p0));
        assert(steering_lut_get_point(i + 1, &a1, &p1));
        assert(a1 > a0);
        assert(p1 < p0);
    }
    assert(!steering_lut_get_point(n, NULL, NULL));

    assert(steering_lut_trim_raw(-2.7f,6,&out)); near(out,-3.24f,.001f);
    assert(steering_lut_trim_raw(-2.7f,14,&out)); near(out,-3.96f,.001f);
    assert(steering_lut_trim_raw(-67.5f,20,&out)); near(out,-67.5f,.001f);
    assert(steering_lut_trim_raw(49.5f,-20,&out)); near(out,49.5f,.001f);
    assert(!steering_lut_trim_raw(NAN,0,&out));
    assert(!steering_lut_trim_raw(0,NAN,&out));
    assert(!steering_lut_trim_raw(0,20.01f,&out));
    assert(!steering_lut_trim_raw(0,0,NULL));
    puts("steering_lut: all tests passed");
    return 0;
}
