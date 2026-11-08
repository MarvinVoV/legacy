/*
 * heading_control_test.c
 *
 * 航向控制器方向与边界测试（对应 2026-11-08 审查报告 IMU-P1-01/02、CTRL-01/10）
 *
 * 符号约定：yaw 逆时针为正；κ 正 = 右转。
 * 前进：yaw 减小 = 车向右偏 → 需要左转（κ 负）→ 修正为负。
 * 倒车：运动学反向，修正符号相反。
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "heading_control.h"

static uint32_t g_tick = 0u;

uint32_t HAL_GetTick(void)
{
    return g_tick;
}

static void near(float actual, float expected, float tol)
{
    if (fabsf(actual - expected) >= tol)
    {
        printf("FAIL: actual=%f expected=%f\n", (double)actual, (double)expected);
    }
    assert(fabsf(actual - expected) < tol);
}

int main(void)
{
    HeadingControl hc;

    /* ---------- 直行：前进方向 ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f); /* 用默认增益 0.02 / 0.002 */
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.1f, 0.0f, true); /* 捕获 ref=0 */

    g_tick = 1010u; /* dt = 10 ms */
    float out = HeadingControl_Apply(&hc, -5.0f, 0.0f, 0.1f, 0.0f, true);
    /* err = -5 → corr ≈ 0.02*(-5) + 0.002*(-5*0.01) = -0.1001 */
    near(out, -0.10f, 0.01f);

    /* ---------- 直行：倒车方向必须反向 ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, -0.1f, 0.0f, true);

    g_tick = 1010u;
    out = HeadingControl_Apply(&hc, -5.0f, 0.0f, -0.1f, 0.0f, true);
    near(out, +0.10f, 0.01f);

    /* ---------- 转弯：角速度反馈（前进） ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.2f, 1.0f, false);
    out = HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.2f, 1.0f, false);
    /* kappaMeas=0，err=1.0，corr = 0.4 被 maxCorr=0.3 限幅 → out = 1.3 */
    near(out, 1.3f, 0.02f);

    /* ---------- 转弯：倒车时角速度反馈同样有效 ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, -0.2f, 1.0f, false);
    /* 倒车 v=-0.2、实际 κ=1.0 时，yawRate = +0.2 rad/s = +11.459°/s */
    out = HeadingControl_Apply(&hc, 0.0f, 11.459f, -0.2f, 1.0f, false);
    near(out, 1.0f, 0.02f); /* 误差为 0，不加修正 */

    /* ---------- 低速：反馈冻结，不应发散 ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.0f, 1.0f, false);
    out = HeadingControl_Apply(&hc, 0.0f, 50.0f, 0.0f, 1.0f, false);
    near(out, 1.0f, 0.02f);

    /* ---------- 最终曲率限幅（CTRL-01） ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.2f, 3.77f, false);
    out = HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.2f, 3.77f, false);
    assert(out <= 3.7701f);

    /* ---------- 死区 ---------- */
    HeadingControl_Init(&hc, 0.0f, 0.0f);
    g_tick = 1000u;
    (void)HeadingControl_Apply(&hc, 0.0f, 0.0f, 0.1f, 0.0f, true);
    g_tick = 1010u;
    out = HeadingControl_Apply(&hc, -0.3f, 0.0f, 0.1f, 0.0f, true); /* < 0.5° 死区 */
    near(out, 0.0f, 0.001f);

    /* ---------- Disable 后重新捕获参考航向（IMU-P2-01） ---------- */
    HeadingControl_Disable(&hc);
    g_tick = 3000u;
    out = HeadingControl_Apply(&hc, 42.0f, 0.0f, 0.1f, 0.0f, true);
    near(out, 0.0f, 0.001f); /* 新参考 = 42°，误差 0 */

    puts("heading_control: all tests passed");
    return 0;
}
