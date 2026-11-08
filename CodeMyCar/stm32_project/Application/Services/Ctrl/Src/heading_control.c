#include "heading_control.h"

#include <math.h>
#include <stddef.h>

#include "stm32h7xx_hal.h"

/* 默认增益：先保守，落地整定后再放大 */
#define HC_KP_ANGLE_DEFAULT (0.020f) /* (1/m)/deg */
#define HC_KI_ANGLE_DEFAULT (0.002f) /* (1/m)/(deg·s) */
#define HC_KP_RATE_DEFAULT (0.400f)  /* (1/m)/(1/m) */
#define HC_KI_RATE_DEFAULT (0.080f)
#define HC_DEADBAND_DEG_DEFAULT (0.5f)
#define HC_DEADBAND_K_DEFAULT (0.05f)
#define HC_MAX_CORR_DEFAULT (0.30f) /* 1/m，约 ±5° 转向 */

/*
 * 符号约定（2026-11-07 实测校准）：
 *   IMU yaw：逆时针（俯视）为正；顺时针转 → yaw 减小。
 *   κ：正 = 右转（脉宽减小）。
 *
 * 角度环：车顺时针偏（yaw < ψ_ref）→ 需要左转（κ 负）
 *         → corr = Kp·(yaw − ψ_ref)          （直接相减，无需翻符号）
 * 角速度环：顺时针转 → yawRate < 0，实际曲率 κ_meas = −ω/v > 0（右转）
 *         → err = κ_ff − κ_meas
 */

static float wrap180(float deg)
{
    while (deg > 180.0f)
    {
        deg -= 360.0f;
    }
    while (deg < -180.0f)
    {
        deg += 360.0f;
    }
    return deg;
}

void HeadingControl_Init(HeadingControl* hc, float kp, float ki)
{
    if (hc == NULL)
    {
        return;
    }
    hc->active = false;
    hc->straightMode = false;
    hc->psiRefDeg = 0.0f;
    hc->integral = 0.0f;
    hc->rateIntegral = 0.0f;
    hc->lastTickMs = 0u;
    hc->motionDirection = 0;
    hc->curveDirection = 0;
    hc->kp = (kp > 0.0f) ? kp : HC_KP_ANGLE_DEFAULT;
    hc->ki = (ki > 0.0f) ? ki : HC_KI_ANGLE_DEFAULT;
    hc->kpRate = HC_KP_RATE_DEFAULT;
    hc->kiRate = HC_KI_RATE_DEFAULT;
    hc->deadbandDeg = HC_DEADBAND_DEG_DEFAULT;
    hc->deadbandK = HC_DEADBAND_K_DEFAULT;
    hc->maxCorr = HC_MAX_CORR_DEFAULT;
    hc->maxKappa = 3.770f; /* STEERING_KAPPA_MAX_1PM */
    hc->lastErrorDeg = 0.0f;
    hc->lastCorr = 0.0f;
}

void HeadingControl_Reset(HeadingControl* hc, float yawDeg)
{
    if (hc == NULL)
    {
        return;
    }
    hc->psiRefDeg = yawDeg;
    hc->integral = 0.0f;
    hc->rateIntegral = 0.0f;
    hc->lastTickMs = 0u;
}

void HeadingControl_Disable(HeadingControl* hc)
{
    if (hc == NULL)
    {
        return;
    }
    hc->active = false;
    hc->straightMode = false; /* 下次调用重新捕获参考航向 */
    hc->psiRefDeg = 0.0f;
    hc->integral = 0.0f;
    hc->rateIntegral = 0.0f;
    hc->lastTickMs = 0u;
    hc->lastCorr = 0.0f;
}

static float clampf(float x, float lo, float hi)
{
    return fmaxf(lo, fminf(hi, x));
}

static float pi_correction(float kp, float ki, float* integral,
                           float error, float dt, float lo, float hi)
{
    const float raw = kp * error + ki * *integral;
    /* The limits include BOTH correction headroom and final actuator limits.
     * Permit integration out of saturation, never further into it. */
    if (dt > 0.0f && (raw < hi || error < 0.0f) &&
                     (raw > lo || error > 0.0f))
    {
        *integral += error * dt;
    }
    return clampf(kp * error + ki * *integral, lo, hi);
}

float HeadingControl_Apply(HeadingControl* hc, float yawDeg, float yawRateDps,
                          float vMps, float kappaFf, bool straight)
{
    if (hc == NULL) { return kappaFf; }
    if (!isfinite(kappaFf)) { HeadingControl_Disable(hc); return 0.0f; }
    const float ff = clampf(kappaFf, -hc->maxKappa, hc->maxKappa);
    if (!isfinite(yawDeg) || !isfinite(yawRateDps) || !isfinite(vMps) ||
        fabsf(vMps) <= 0.03f)
    {
        /* No observable yaw dynamics at rest. Do not learn an integral or
         * flip the steering correction on encoder noise around zero. */
        HeadingControl_Disable(hc);
        return ff;
    }
    const uint32_t now = HAL_GetTick();
    float dt = hc->active ? (float)(uint32_t)(now-hc->lastTickMs)*0.001f : 0.0f;
    if (dt > 0.2f) { dt = 0.0f; }
    hc->lastTickMs = now;
    hc->active = true;
    const int8_t direction = vMps > 0.0f ? 1 : -1;
    if (hc->motionDirection != direction)
    {
        hc->integral = 0.0f;
        hc->rateIntegral = 0.0f;
        hc->motionDirection = direction;
    }
    const float lo = fmaxf(-hc->maxCorr, -hc->maxKappa-ff);
    const float hi = fminf(hc->maxCorr, hc->maxKappa-ff);
    float corr;
    if (straight)
    {
        if (!hc->straightMode)
        {
            hc->psiRefDeg = yawDeg;
            hc->integral = 0.0f;
            hc->rateIntegral = 0.0f;
        }
        hc->straightMode = true;
        hc->curveDirection = 0;
        float error = direction * wrap180(yawDeg-hc->psiRefDeg);
        hc->lastErrorDeg = error;
        if (fabsf(error) < hc->deadbandDeg) { error = 0.0f; }
        corr = pi_correction(hc->kp, hc->ki, &hc->integral, error, dt, lo, hi);
    }
    else
    {
        const int8_t curve = ff > 0.0f ? 1 : (ff < 0.0f ? -1 : 0);
        if (hc->straightMode || curve != hc->curveDirection)
        {
            hc->integral = 0.0f;
            hc->rateIntegral = 0.0f;
        }
        hc->straightMode = false;
        hc->curveDirection = curve;
        float error = ff + yawRateDps * 0.017453293f / vMps;
        hc->lastErrorDeg = error;
        if (fabsf(error) < hc->deadbandK) { error = 0.0f; }
        corr = pi_correction(hc->kpRate, hc->kiRate, &hc->rateIntegral,
                             error, dt, lo, hi);
    }
    const float out = clampf(ff+corr, -hc->maxKappa, hc->maxKappa);
    hc->lastCorr = out-ff;
    return out;
}
