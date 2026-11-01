/*
 * motion_goal_test.c — 目标型运动控制 host 单元测试（纯 C）
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "motion_goal.h"

static const MotionGoalConfig CFG = {
    .wheelbaseMm     = 144.0f,
    .trackWidthMm    = 167.0f,
    .wheelRadiusMm   = 32.5f,
    .maxSteerDeg     = 45.0f,
    .maxRpm          = 300.0f,
    .maxAngularRadps = 8.2f,
    .decelMmps2      = 300.0f,
    .minTurnRadiusMm = 144.0f,
    .allowPivot      = true,
};

static MotionGoalState start(int16_t turn, int16_t dist, uint16_t speed)
{
    MotionGoalState st;
    MotionGoalTarget t = {.turnDdeg = turn, .distanceMm = dist, .speedMmps = speed};
    memset(&st, 0, sizeof(st));
    assert(motion_goal_start(&st, &t, &CFG, 0) == 0);
    return st;
}

/* 用左右轮增量把航向/距离推进到指定值 */
static void advance(MotionGoalState* st, float distMm, float turnDeg)
{
    const float dTheta = turnDeg * (float)M_PI / 180.0f;
    const float diff   = dTheta * CFG.trackWidthMm; /* left - right */
    motion_goal_accumulate(st, distMm + diff / 2.0f, distMm - diff / 2.0f, &CFG);
}

int main(void)
{
    /* 1. 前进右转：舵机右打 */
    MotionGoalState st = start(300, 1000, 300);
    MotionGoalOutput out = motion_goal_step(&st, &CFG, 0);
    assert(out.error == 0 && !out.finished);
    assert(out.steerAngleDeg > 1.0f);

    /* 2. 前进左转：舵机左打 */
    st  = start(-300, 1000, 300);
    out = motion_goal_step(&st, &CFG, 0);
    assert(out.steerAngleDeg < -1.0f);

    /* 3. 后退右转：车头右转要求前轮左打 */
    st  = start(300, -1000, 300);
    out = motion_goal_step(&st, &CFG, 0);
    assert(out.steerAngleDeg < -1.0f);

    /* 4. 原地右转：顺时针 = 左轮前进、右轮后退 */
    st  = start(900, 0, 300);
    out = motion_goal_step(&st, &CFG, 0);
    assert(out.pivot);
    assert(out.leftRpm > 0.0f && out.rightRpm < 0.0f);

    /* 5. 270° 不回绕：180° 时未完成，270° 时完成 */
    st = start(2700, 0, 300);
    advance(&st, 0.0f, 180.0f);
    out = motion_goal_step(&st, &CFG, 100);
    assert(!out.finished);
    advance(&st, 0.0f, 90.0f);
    out = motion_goal_step(&st, &CFG, 200);
    assert(out.finished);

    /* 6. 曲率半径不足：100mm 走 90° -> R=63.7mm < 144mm */
    {
        MotionGoalState bad;
        MotionGoalTarget t = {.turnDdeg = 900, .distanceMm = 100, .speedMmps = 300};
        memset(&bad, 0, sizeof(bad));
        assert(motion_goal_start(&bad, &t, &CFG, 0) == 3);
    }

    /* 7. 距离先到、航向未到：收尾超时必须报错，不能伪装 DONE */
    st = start(900, 1000, 300);
    advance(&st, 1000.0f, 0.0f);
    out = motion_goal_step(&st, &CFG, 2000);
    assert(!out.finished);
    assert(out.pivot);
    out = motion_goal_step(&st, &CFG, 3600);
    assert(out.finished && out.error == 5);

    /* 8. 距离与航向都在容差内：立即完成 */
    st = start(300, 1000, 300);
    advance(&st, 995.0f, 30.0f);
    out = motion_goal_step(&st, &CFG, 300);
    assert(out.finished);

    /* 9. 纯旋转只看航向：平均距离漂移不应阻塞完成 */
    st = start(900, 0, 300);
    motion_goal_accumulate(&st, 262.4f, 0.0f, &CFG); /* 航向 90°，平均距离 131mm */
    out = motion_goal_step(&st, &CFG, 100);
    assert(out.finished);

    /* 10. 无有效反馈时硬超时仍然终止（Review Focus #5） */
    st = start(900, 0, 300);
    out = motion_goal_step(&st, &CFG, 31000);
    assert(out.finished && out.error == 5);

    /* 11. 短动作的本地兜底不能被固定 30s 下限拖长 */
    st = start(0, 100, 100);
    out = motion_goal_step(&st, &CFG, 6000);
    assert(out.finished && out.error == 5);

    /* 12. 反馈反向（两轮都后退）时仍朝目标方向纠正 */
    st = start(900, 0, 300);
    motion_goal_accumulate(&st, -100.0f, -100.0f, &CFG);
    out = motion_goal_step(&st, &CFG, 100);
    assert(out.pivot && out.leftRpm > 0.0f && out.rightRpm < 0.0f);

    /* 13. 最小转弯半径独立于轴距 */
    {
        MotionGoalConfig cfg2 = CFG;
        cfg2.minTurnRadiusMm = 200.0f;
        MotionGoalState s2;
        MotionGoalTarget t2 = {.turnDdeg = 900, .distanceMm = 275, .speedMmps = 300};
        memset(&s2, 0, sizeof(s2));
        assert(motion_goal_start(&s2, &t2, &cfg2, 0) == 3);
    }

    /* 14. 展示航向归一化到 (-180, 180] */
    assert(fabsf(motion_normalize_deg(270.0f) - (-90.0f)) < 0.01f);
    assert(fabsf(motion_normalize_deg(-270.0f) - 90.0f) < 0.01f);
    assert(fabsf(motion_normalize_deg(180.0f) - 180.0f) < 0.01f);

    /* 15. Ackermann 配置拒绝纯原地旋转。 */
    {
        MotionGoalConfig ackermann = CFG;
        ackermann.allowPivot = false;
        MotionGoalState rejected;
        MotionGoalTarget t = {.turnDdeg = 900, .distanceMm = 0, .speedMmps = 300};
        memset(&rejected, 0, sizeof(rejected));
        assert(motion_goal_start(&rejected, &t, &ackermann, 0) == 3);
    }

    /* 16. Ackermann 到达距离但航向超差时必须 ERROR，不能反向双轮收尾。 */
    {
        MotionGoalConfig ackermann = CFG;
        ackermann.allowPivot = false;
        MotionGoalState mismatch;
        MotionGoalTarget t = {.turnDdeg = 900, .distanceMm = 1000, .speedMmps = 300};
        memset(&mismatch, 0, sizeof(mismatch));
        assert(motion_goal_start(&mismatch, &t, &ackermann, 0) == 0);
        advance(&mismatch, 1000.0f, 0.0f);
        out = motion_goal_step(&mismatch, &ackermann, 2000);
        assert(out.finished && out.error == 5 && !out.pivot);
    }

    /* 17. 接近终点时航向残留使所需转角超过标称上限：钳位到上限继续，
       不能以 geometry 中止（实车直线 GOAL 曾频繁触发）。 */
    {
        MotionGoalConfig ackermann = CFG;
        ackermann.maxSteerDeg = 25.0f;
        ackermann.minTurnRadiusMm = 308.8f;
        ackermann.allowPivot = false;
        MotionGoalState nearEnd;
        MotionGoalTarget t = {.turnDdeg = 0, .distanceMm = 200, .speedMmps = 200};
        memset(&nearEnd, 0, sizeof(nearEnd));
        assert(motion_goal_start(&nearEnd, &t, &ackermann, 0) == 0);
        advance(&nearEnd, 190.0f, 2.0f); /* 还剩 10mm，航向偏了 2° */
        out = motion_goal_step(&nearEnd, &ackermann, 100);
        assert(!out.finished);
        assert(out.error == 0);
        assert(fabsf(out.steerAngleDeg) <= 25.0f + 1e-3f);
        assert(fabsf(out.steerAngleDeg) > 20.0f); /* 仍按最大能力纠正 */
    }

    printf("motion_goal: all tests passed\n");
    return 0;
}
