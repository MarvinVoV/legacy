/*
 * safety_service.c
 *
 * 超声波安全策略实现（纯 C）。
 */

#include "safety_service.h"

#include <stddef.h>

/*------------------------------------------------------------------------------
 * 内部工具
 *---------------------------------------------------------------------------*/

static void sort_i16(int16_t* a, uint8_t n)
{
    /* 插入排序：n ≤ 5，足够 */
    for (uint8_t i = 1; i < n; i++)
    {
        const int16_t key = a[i];
        int16_t       j   = (int16_t)i - 1;
        while (j >= 0 && a[j] > key)
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/**
 * @brief 当前窗口的中值
 * @return 中值；窗口为空或中值无效（-1）时返回 -1
 */
static int16_t channel_median(const SafetyChannel* ch)
{
    if (ch->count == 0)
    {
        return SAFETY_INVALID_DISTANCE;
    }

    int16_t tmp[SAFETY_SAMPLE_COUNT];
    for (uint8_t i = 0; i < ch->count; i++)
    {
        tmp[i] = ch->samples[i];
    }
    sort_i16(tmp, ch->count);

    const int16_t median = tmp[ch->count / 2];
    return (median > 0) ? median : SAFETY_INVALID_DISTANCE;
}

/**
 * @brief 距离 → 速度比例
 *
 * d ≥ slow → 1.0
 * stop < d < slow → (d - stop) / (slow - stop)
 * d ≤ stop → 0.0
 */
static float distance_to_scale(const SafetyChannel* ch, int16_t distanceCm)
{
    const float d = (float)distanceCm - (float)ch->offsetCm;

    if (d >= (float)ch->slowCm)
    {
        return 1.0f;
    }
    if (d <= (float)ch->stopCm)
    {
        return 0.0f;
    }
    return (d - (float)ch->stopCm) / ((float)ch->slowCm - (float)ch->stopCm);
}

/*------------------------------------------------------------------------------
 * 公共 API
 *---------------------------------------------------------------------------*/

void safety_channel_init(SafetyChannel* ch, const bool enabled, const int16_t offsetCm,
                         const uint16_t stopCm, const uint16_t slowCm)
{
    if (ch == NULL)
    {
        return;
    }

    ch->scale      = 1.0f;
    ch->distanceCm = SAFETY_INVALID_DISTANCE;
    ch->enabled    = enabled ? 1u : 0u;
    ch->everValid  = 0u;
    ch->fault      = 0u;
    ch->offsetCm   = offsetCm;
    ch->stopCm     = stopCm;
    ch->slowCm     = slowCm;
    ch->count      = 0u;
    ch->index      = 0u;

    for (uint8_t i = 0; i < SAFETY_SAMPLE_COUNT; i++)
    {
        ch->samples[i] = SAFETY_INVALID_DISTANCE;
    }
}

void safety_channel_update(SafetyChannel* ch, int16_t rawDistanceCm)
{
    if (ch == NULL || !ch->enabled)
    {
        return;
    }

    /* 入窗口；无效值记 -1，由中值自然过滤 */
    ch->samples[ch->index] = (rawDistanceCm > 0) ? rawDistanceCm : SAFETY_INVALID_DISTANCE;
    ch->index              = (uint8_t)((ch->index + 1u) % SAFETY_SAMPLE_COUNT);
    if (ch->count < SAFETY_SAMPLE_COUNT)
    {
        ch->count++;
    }

    if (rawDistanceCm > 0)
    {
        ch->everValid = 1u;
    }

    const int16_t median = channel_median(ch);
    ch->distanceCm       = median;

    if (median > 0)
    {
        ch->fault = 0u;
        ch->scale = distance_to_scale(ch, median);
        return;
    }

    /* 无有效中值 */
    if (!ch->everValid)
    {
        /* 从未测到过：视为未安装，完全放行 */
        ch->fault = 0u;
        ch->scale = 1.0f;
        return;
    }

    /* 工作过之后丢数据：fail-safe 停车 */
    ch->fault = 1u;
    ch->scale = 0.0f;
}

float safety_governor_apply(const SafetyChannel* front, const SafetyChannel* rear,
                            float* leftRpm, float* rightRpm)
{
    if (leftRpm == NULL || rightRpm == NULL)
    {
        return 1.0f;
    }

    const float linear = (*leftRpm + *rightRpm) * 0.5f;

    /* 原地转（或极低速）：不缩放 */
    if (linear > -SAFETY_PIVOT_DEADBAND_RPM && linear < SAFETY_PIVOT_DEADBAND_RPM)
    {
        return 1.0f;
    }

    const SafetyChannel* ch = (linear > 0.0f) ? front : rear;
    if (ch == NULL || !ch->enabled)
    {
        return 1.0f;
    }

    const float scale = ch->scale;
    *leftRpm *= scale;
    *rightRpm *= scale;
    return scale;
}
