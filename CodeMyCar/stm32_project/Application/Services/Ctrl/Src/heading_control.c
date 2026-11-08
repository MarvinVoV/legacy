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

float HeadingControl_Apply(HeadingControl* hc, float yawDeg, float yawRateDps,
                           float vMps, float kappaFf, bool straight)
{
    if (hc == NULL)
    {
        return kappaFf;
    }
    hc->active = true;

    /* dt（秒），首帧或时间跳变时清零积分 */
    const uint32_t now = HAL_GetTick();
    float dt = 0.0f;
    if (hc->lastTickMs != 0u && now > hc->lastTickMs)
    {
        dt = (float)(now - hc->lastTickMs) * 0.001f;
    }
    hc->lastTickMs = now;
    if (dt > 0.2f)
    {
        dt = 0.0f; /* 长时间未调用（入弯/切模式），不积分 */
    }

    float corr = 0.0f;

    if (straight)
    {
        if (!hc->straightMode)
        {
            /* 刚进入直行：以当前航向为基准，并清掉转弯段的角速度积分 */
            hc->straightMode = true;
            hc->psiRefDeg = yawDeg;
            hc->integral = 0.0f;
            hc->rateIntegral = 0.0f;
        }

        /* 倒车时运动学方向反转：同样的舵角产生的航向变化相反 */
        const float dir = (vMps >= 0.0f) ? 1.0f : -1.0f;
        float err = dir * wrap180(yawDeg - hc->psiRefDeg);
        hc->lastErrorDeg = err;

        /* 死区：吸收约 4° 回差带来的抖动 */
        if (fabsf(err) < hc->deadbandDeg)
        {
            err = 0.0f;
        }

        float corrRaw = hc->kp * err + hc->ki * hc->integral;

        /* 抗饱和 */
        if ((corrRaw < hc->maxCorr) && (corrRaw > -hc->maxCorr) && dt > 0.0f)
        {
            hc->integral += err * dt;
        }
        corr = hc->kp * err + hc->ki * hc->integral;
    }
    else
    {
        if (hc->straightMode)
        {
            /* 直行 → 转弯：不携带直行段的角度积分 */
            hc->straightMode = false;
            hc->integral = 0.0f;
            hc->rateIntegral = 0.0f;
        }

        /* 由角速度反推实际曲率：ψ̇ = -v·κ → κ = -ψ̇/v（v 可为负，倒车同样成立） */
        float kappaMeas = kappaFf;
        if (fabsf(vMps) > 0.03f)
        {
            kappaMeas = -(yawRateDps * 0.017453293f) / vMps;
        }
        else
        {
            /* 速度过低时反馈不可用：冻结角速度积分，仅保留已有修正量 */
            hc->rateIntegral *= 0.98f;
        }
        float err = kappaFf - kappaMeas;
        hc->lastErrorDeg = err;

        if (fabsf(err) < hc->deadbandK)
        {
            err = 0.0f;
        }

        float corrRaw = hc->kpRate * err + hc->kiRate * hc->rateIntegral;
        if ((corrRaw < hc->maxCorr) && (corrRaw > -hc->maxCorr) && dt > 0.0f)
        {
            hc->rateIntegral += err * dt;
        }
        corr = hc->kpRate * err + hc->kiRate * hc->rateIntegral;
    }

    if (corr > hc->maxCorr)
    {
        corr = hc->maxCorr;
    }
    if (corr < -hc->maxCorr)
    {
        corr = -hc->maxCorr;
    }
    hc->lastCorr = corr;

    float out = kappaFf + corr;
    if (out > hc->maxKappa)
    {
        out = hc->maxKappa;
    }
    if (out < -hc->maxKappa)
    {
        out = -hc->maxKappa;
    }
    return out;
}
