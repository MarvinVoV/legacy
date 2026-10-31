/*
 * ultrasonic_safety_test.c
 *
 * 超声波安全策略的 host 单元测试（纯 C，无 HAL）：
 * - 距离→速度比例曲线
 * - 中值滤波（单点噪声抑制）
 * - 故障判定（从未测到 = 未安装；工作中丢数据 = 停车）
 * - 分方向限速（前进用前向、倒车用后向、原地转不缩放）
 */

#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#include "safety_service.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabsf((a) - (b)) <= (eps))

static void feed(SafetyChannel* ch, int16_t raw, int times)
{
    for (int i = 0; i < times; i++)
    {
        safety_channel_update(ch, raw);
    }
}

/* 1. 比例曲线：100cm 以上畅通，25cm 以下停车，中间线性 */
static void test_scale_curve(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 0, 25, 100);

    feed(&ch, 150, 5);
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);

    feed(&ch, 100, 5);
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);

    feed(&ch, 62, 5); /* (62-25)/75 = 0.4933 */
    CHECK_NEAR(ch.scale, 0.4933f, 0.01f);

    feed(&ch, 25, 5);
    CHECK_NEAR(ch.scale, 0.0f, 1e-6f);

    feed(&ch, 10, 5);
    CHECK_NEAR(ch.scale, 0.0f, 1e-6f);
}

/* 2. 中值滤波：单个误读不改变结果 */
static void test_median_rejects_spike(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 0, 25, 100);

    feed(&ch, 120, 5);
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);

    safety_channel_update(&ch, 5); /* 单点噪声 */
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);

    safety_channel_update(&ch, 6); /* 两点噪声 */
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);
}

/* 3. 从未测到有效值 = 未安装，不影响运动 */
static void test_never_valid_is_disabled(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 0, 25, 100);

    feed(&ch, -1, 10);
    CHECK(ch.fault == 0);
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);
}

/* 4. 工作过之后丢数据 = 故障，scale=0 */
static void test_fault_after_working(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 0, 25, 100);

    feed(&ch, 120, 5);
    CHECK(ch.fault == 0);

    safety_channel_update(&ch, -1);
    CHECK(ch.fault == 0);
    safety_channel_update(&ch, -1);
    safety_channel_update(&ch, -1);

    CHECK(ch.fault == 1);
    CHECK_NEAR(ch.scale, 0.0f, 1e-6f);
}

/* 5. 传感器未启用时完全放行 */
static void test_disabled_channel_passes(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, false, 0, 25, 100);

    feed(&ch, 10, 5);
    CHECK_NEAR(ch.scale, 1.0f, 1e-6f);
}

/* 6. 安装偏移：传感器内缩 5cm 时，读数 30cm 等效 25cm */
static void test_install_offset(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 5, 25, 100);

    feed(&ch, 30, 5);
    CHECK_NEAR(ch.scale, 0.0f, 1e-6f);

    feed(&ch, 80, 5); /* 80-5=75 → (75-25)/75 = 0.667 */
    CHECK_NEAR(ch.scale, 0.6667f, 0.01f);
}

/* 7. 前进只看前向，倒车只看后向 */
static void test_governor_direction(void)
{
    SafetyChannel front, rear;
    safety_channel_init(&front, true, 0, 25, 100);
    safety_channel_init(&rear, true, 0, 25, 100);

    feed(&front, 62, 5);  /* 前向约 0.49 */
    feed(&rear, 200, 5);  /* 后方畅通 */

    float l = 30.0f, r = 30.0f;
    float s = safety_governor_apply(&front, &rear, &l, &r);
    CHECK_NEAR(s, 0.4933f, 0.01f);
    CHECK_NEAR(l, 30.0f * s, 1e-4f);
    CHECK_NEAR(r, 30.0f * s, 1e-4f);

    /* 倒车：前向很近但不影响 */
    l = -30.0f;
    r = -30.0f;
    s = safety_governor_apply(&front, &rear, &l, &r);
    CHECK_NEAR(s, 1.0f, 1e-6f);
    CHECK_NEAR(l, -30.0f, 1e-6f);

    /* 原地转：不缩放 */
    l = 30.0f;
    r = -30.0f;
    s = safety_governor_apply(&front, &rear, &l, &r);
    CHECK_NEAR(s, 1.0f, 1e-6f);
    CHECK_NEAR(l, 30.0f, 1e-6f);
    CHECK_NEAR(r, -30.0f, 1e-6f);
}

/* 8. 后方近、前向畅通：倒车被限速 */
static void test_governor_rear(void)
{
    SafetyChannel front, rear;
    safety_channel_init(&front, true, 0, 25, 100);
    safety_channel_init(&rear, true, 0, 25, 100);

    feed(&front, 200, 5);
    feed(&rear, 40, 5); /* (40-25)/75 = 0.2 */

    float l = -20.0f, r = -20.0f;
    float s = safety_governor_apply(&front, &rear, &l, &r);
    CHECK_NEAR(s, 0.2f, 0.02f);
    CHECK(l > -20.0f && l < 0.0f);
}

/* 9. 硬停止判据：进入停止距离 / 故障时刹车，离开迟滞距离后解除 */
static void test_hard_stop_predicate(void)
{
    SafetyChannel ch;
    safety_channel_init(&ch, true, 0, 8, 40);

    feed(&ch, 30, 5);
    CHECK(!safety_channel_blocked(&ch));
    CHECK(safety_channel_clear(&ch, 12));

    feed(&ch, 8, 5);
    CHECK(safety_channel_blocked(&ch));
    CHECK(!safety_channel_clear(&ch, 12));

    feed(&ch, 10, 5); /* 8~12 之间：保持刹车（迟滞） */
    CHECK(!safety_channel_blocked(&ch));
    CHECK(!safety_channel_clear(&ch, 12));

    feed(&ch, 13, 5);
    CHECK(safety_channel_clear(&ch, 12));

    /* 故障（工作过之后丢数据）也刹车 */
    safety_channel_update(&ch, -1);
    safety_channel_update(&ch, -1);
    safety_channel_update(&ch, -1);
    CHECK(safety_channel_blocked(&ch));

    /* 未安装：永远不刹车 */
    SafetyChannel off;
    safety_channel_init(&off, false, 0, 8, 40);
    feed(&off, -1, 10);
    CHECK(!safety_channel_blocked(&off));
}

/* 10. 未启用的方向不参与（后向传感器未安装） */
static void test_governor_rear_not_installed(void)
{
    SafetyChannel front, rear;
    safety_channel_init(&front, true, 0, 25, 100);
    safety_channel_init(&rear, false, 0, 25, 100);

    feed(&front, 200, 5);

    float l = -20.0f, r = -20.0f;
    float s = safety_governor_apply(&front, &rear, &l, &r);
    CHECK_NEAR(s, 1.0f, 1e-6f);
}

int main(void)
{
    test_scale_curve();
    test_median_rejects_spike();
    test_never_valid_is_disabled();
    test_fault_after_working();
    test_disabled_channel_passes();
    test_install_offset();
    test_governor_direction();
    test_governor_rear();
    test_governor_rear_not_installed();
    test_hard_stop_predicate();

    if (g_failures == 0)
    {
        printf("ultrasonic_safety_test: ALL PASS\n");
        return 0;
    }
    printf("ultrasonic_safety_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
