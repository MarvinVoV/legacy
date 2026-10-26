/*
 * motion_kinematics_test.c — host 单元测试（纯 C，无 STM32 依赖）
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "motion_kinematics.h"

#define WHEELBASE_M 0.144f
#define TRACK_M     0.167f
#define RADIUS_M    0.0325f
#define MAX_RPM     MOTION_MAX_RPM

static int near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

int main(void)
{
    float l, r;

    /* 直行：无转向死区，左右同速 */
    motion_calc_drive(0.5f, 0.0f, WHEELBASE_M, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(near(l, 146.95f, 0.5f));
    assert(near(l, r, 0.01f));

    /* 右转（steer>0）：左轮外侧，应更快 */
    motion_calc_drive(0.5f, 20.0f, WHEELBASE_M, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(l > r);
    assert(near(l, 177.9f, 0.8f));
    assert(near(r, 115.9f, 0.8f));

    /* 左转（steer<0）：右轮外侧 */
    motion_calc_drive(0.5f, -20.0f, WHEELBASE_M, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(r > l);
    assert(near(l, 115.9f, 0.8f));
    assert(near(r, 177.9f, 0.8f));

    /* 倒车 + 右转：符号保持，内外关系不变 */
    motion_calc_drive(-0.5f, 20.0f, WHEELBASE_M, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(l < r); /* 两者为负，外侧左轮绝对值更大 */
    assert(near(l, -177.9f, 0.8f));
    assert(near(r, -115.9f, 0.8f));

    /* 超速限幅 */
    motion_calc_drive(1.5f, 0.0f, WHEELBASE_M, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(near(l, MAX_RPM, 0.01f));
    assert(near(r, MAX_RPM, 0.01f));

    /* 差速原地旋转：v=0, ω=1 */
    motion_calc_differential(0.0f, 1.0f, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(near(l, -24.54f, 0.5f));
    assert(near(r, 24.54f, 0.5f));

    /* 差速限幅：一侧超 300 被 clamp */
    motion_calc_differential(1.0f, 8.2f, TRACK_M, RADIUS_M, MAX_RPM, &l, &r);
    assert(near(r, MAX_RPM, 0.01f));
    assert(l < MAX_RPM);

    /* 非法半径保护 */
    motion_calc_drive(1.0f, 0.0f, WHEELBASE_M, TRACK_M, 0.0f, MAX_RPM, &l, &r);
    assert(l == 0.0f && r == 0.0f);

    printf("motion_kinematics: all tests passed\n");
    return 0;
}
