#include "motion_goal.h"

#include <math.h>
#include <string.h>

#include "motion_kinematics.h"

#define DEG2RAD 0.017453292519943295f
#define RAD2DEG 57.29577951308232f

#define TOL_DISTANCE_MM 10.0f
#define TOL_TURN_DEG    2.0f
#define ENDGAME_MAX_MS  1500u
#define MIN_TIMEOUT_MS  30000u
#define PIVOT_GAIN      3.0f
#define TURN_RATE_DPS   90.0f /* 仅用于时长预估 */

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
    const bool distance_done =
        (s->target.distanceMm == 0) || (fabsf(sRem) <= TOL_DISTANCE_MM);
    if (distance_done && fabsf(thRem) <= TOL_TURN_DEG)
    {
        s->finished  = true;
        s->finishMs  = nowMs;
        out.finished = true;
        return out;
    }

    /* 全局兜底：预估时长的 2 倍 + 10s，至少 30s */
    const uint32_t limit_ms = estimate_ms(&s->target) * 2u + 10000u;
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
        s->inEndgame      = true;
        s->endgameStartMs = nowMs;
    }

    if (s->inEndgame && nowMs - s->endgameStartMs > ENDGAME_MAX_MS)
    {
        s->finished  = true;
        s->finishMs  = nowMs;
        out.finished = true;
        return out;
    }

    const bool pivot = s->inEndgame || s->target.distanceMm == 0;

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
    const float delta = atanf((cfg->wheelbaseMm / 1000.0f) * kappa) * RAD2DEG;
    if (fabsf(delta) > cfg->maxSteerDeg)
    {
        s->error     = 3;
        s->finished  = true;
        s->finishMs  = nowMs;
        out.error    = 3;
        out.finished = true;
        return out;
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
