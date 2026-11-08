#include "motion_goal.h"

#include <math.h>
#include <string.h>

#include "motion_kinematics.h"

#define DEG2RAD 0.017453292519943295f
#define RAD2DEG 57.29577951308232f

#define TOL_DISTANCE_MM 5.0f
#define TOL_TURN_DEG    2.0f
#define ENDGAME_MAX_MS  1500u
#define MIN_TIMEOUT_MS  5000u
#define TIMEOUT_GRACE_MS 3000u
#define PIVOT_GAIN      3.0f
#define TURN_RATE_DPS   90.0f /* 仅用于时长预估 */

bool motion_goal_within_tolerance(const MotionGoalState* s)
{
    if (s == NULL || !isfinite(s->distanceTravelMm) || !isfinite(s->headingTravelDeg)) { return false; }
    return (s->target.distanceMm == 0 ||
            fabsf((float)s->target.distanceMm-s->distanceTravelMm) <= TOL_DISTANCE_MM) &&
           fabsf((float)s->target.turnDdeg*.1f-s->headingTravelDeg) <= TOL_TURN_DEG;
}

static uint32_t estimate_ms(const MotionGoalTarget* t)
{
    float ms = fabsf((float)t->distanceMm) / (float)t->speedMmps * 1000.0f;
    ms += fabsf((float)t->turnDdeg * 0.1f) / TURN_RATE_DPS * 1000.0f;
    return (uint32_t)ms;
}

int motion_goal_start(MotionGoalState* s, const MotionGoalTarget* t,
                      const MotionGoalConfig* cfg, uint32_t nowMs)
{
    if (s == NULL || t == NULL || cfg == NULL)
    {
        return 1;
    }
    if (t->turnDdeg < -3600 || t->turnDdeg > 3600 ||
        t->distanceMm < -5000 || t->distanceMm > 5000 ||
        t->speedMmps < 1 || t->speedMmps > 600)
    {
        return 1;
    }
    if (t->turnDdeg == 0 && t->distanceMm == 0)
    {
        return 1;
    }
    if (t->distanceMm == 0 && t->turnDdeg != 0 && !cfg->allowPivot)
    {
        return 3;
    }

    /* 曲率半径 = 弧长 / 圆心角；不足最小转弯半径则拒绝 */
    if (t->distanceMm != 0 && t->turnDdeg != 0)
    {
        const float theta_rad = fabsf((float)t->turnDdeg * 0.1f * DEG2RAD);
        const float radius_mm = fabsf((float)t->distanceMm) / theta_rad;
        const float min_radius =
            (cfg->minTurnRadiusMm > 0.0f) ? cfg->minTurnRadiusMm : cfg->wheelbaseMm;
        if (radius_mm < min_radius)
        {
            return 3;
        }
    }

    memset(s, 0, sizeof(*s));
    s->target     = *t;
    s->startMs    = nowMs;
    s->active     = true;
    s->finished   = false;
    s->error      = 0;
    s->inEndgame  = false;
    return 0;
}

void motion_goal_accumulate(MotionGoalState* s, float deltaLeftMm,
                            float deltaRightMm, const MotionGoalConfig* cfg)
{
    if (s == NULL || cfg == NULL || cfg->trackWidthMm <= 0.0f)
    {
        return;
    }
    /* 右转时左轮（外侧）行程更大 -> 航向变化为正 */
    s->headingTravelDeg += (deltaLeftMm - deltaRightMm) / cfg->trackWidthMm * RAD2DEG;
    s->distanceTravelMm += (deltaLeftMm + deltaRightMm) * 0.5f;
}

MotionGoalOutput motion_goal_step(MotionGoalState* s, const MotionGoalConfig* cfg,
                                  uint32_t nowMs)
{
    MotionGoalOutput out;
    memset(&out, 0, sizeof(out));

    if (s == NULL || cfg == NULL || !s->active || s->finished)
    {
        out.finished = true;
        return out;
    }

    const float sRem  = (float)s->target.distanceMm - s->distanceTravelMm;
    const float thRem = (float)s->target.turnDdeg * 0.1f - s->headingTravelDeg;

    /* 纯旋转没有距离目标：平均轮程的微小漂移不应阻塞完成 */
    if (motion_goal_within_tolerance(s))
    {
        s->finished  = true;
        s->finishMs  = nowMs;
        out.finished = true;
        return out;
    }

    /* 全局兜底：短动作也应在秒级失败，不能被固定 30s 下限拖住。 */
    const uint32_t limit_ms = estimate_ms(&s->target) * 2u + TIMEOUT_GRACE_MS;
    if (nowMs - s->startMs > (limit_ms > MIN_TIMEOUT_MS ? limit_ms : MIN_TIMEOUT_MS))
    {
        s->error     = 5;
        s->finished  = true;
        s->finishMs  = nowMs;
        out.error    = 5;
        out.finished = true;
        return out;
    }

    /* 只有“有距离目标且距离已到点”才进入收尾修正；纯旋转不走收尾 */
    if (s->target.distanceMm != 0 && !s->inEndgame &&
        fabsf(sRem) <= TOL_DISTANCE_MM)
    {
        if (!cfg->allowPivot)
        {
            s->error     = 5;
            s->finished  = true;
            s->finishMs  = nowMs;
            out.error    = 5;
            out.finished = true;
            return out;
        }
        s->inEndgame      = true;
        s->endgameStartMs = nowMs;
    }

    if (s->inEndgame && nowMs - s->endgameStartMs > ENDGAME_MAX_MS)
    {
        s->error     = 5;
        s->finished  = true;
        s->finishMs  = nowMs;
        out.error    = 5;
        out.finished = true;
        return out;
    }

    const bool pivot = cfg->allowPivot &&
                       (s->inEndgame || s->target.distanceMm == 0);

    if (pivot)
    {
        /* 右转 = 顺时针，对应 CCW 角速度为负 */
        float omega = -(thRem * DEG2RAD) * PIVOT_GAIN;
        if (omega > cfg->maxAngularRadps)  omega = cfg->maxAngularRadps;
        if (omega < -cfg->maxAngularRadps) omega = -cfg->maxAngularRadps;

        motion_calc_differential(0.0f, omega,
                                 cfg->trackWidthMm / 1000.0f,
                                 cfg->wheelRadiusMm / 1000.0f,
                                 cfg->maxRpm,
                                 &out.leftRpm, &out.rightRpm);
        out.steerAngleDeg = 0.0f;
        out.pivot         = true;
        return out;
    }

    float v_mmps = (float)s->target.speedMmps;
    const float decel_limit = sqrtf(2.0f * cfg->decelMmps2 * fabsf(sRem));
    if (decel_limit < v_mmps)
    {
        v_mmps = decel_limit;
    }
    if (sRem < 0.0f)
    {
        v_mmps = -v_mmps;
    }

    const float kappa = (thRem * DEG2RAD) / (sRem / 1000.0f);
    float delta = atanf((cfg->wheelbaseMm / 1000.0f) * kappa) * RAD2DEG;
    if (fabsf(delta) > cfg->maxSteerDeg)
    {
        /* 航向残差 ÷ 很小的剩余距离会让曲率趋于无穷。终点附近这是正常的
           修正需求，不是几何不可行：标称上限（25°）远小于执行器能力，
           钳位后按最大转向继续纠正即可；"距离到、航向不到"仍由上面的
           Ackermann 分支判 ERROR，整体不可行的路径在 start 时已拒绝。 */
        delta = (delta > 0.0f) ? cfg->maxSteerDeg : -cfg->maxSteerDeg;
    }

    motion_calc_drive(v_mmps / 1000.0f, delta,
                      cfg->wheelbaseMm / 1000.0f,
                      cfg->trackWidthMm / 1000.0f,
                      cfg->wheelRadiusMm / 1000.0f,
                      cfg->maxRpm,
                      &out.leftRpm, &out.rightRpm);
    out.steerAngleDeg = delta;
    return out;
}

float motion_normalize_deg(float deg)
{
    while (deg > 180.0f)
    {
        deg -= 360.0f;
    }
    while (deg <= -180.0f)
    {
        deg += 360.0f;
    }
    return deg;
}
