/*
 * safety_service.h
 *
 * 超声波安全策略（纯 C，可 host 测试，无 HAL 依赖）
 *
 * 职责：
 *  - 对单路超声波距离做中值滤波、安装偏移修正、有效性/故障判定
 *  - 把距离映射为 0..1 的速度比例 scale
 *  - 按运动方向（前进/倒车）选择对应的 scale 作用到左右轮目标转速
 */

#ifndef SERVICES_SAFETY_INC_SAFETY_SERVICE_H_
#define SERVICES_SAFETY_INC_SAFETY_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

/** 中值滤波窗口（奇数） */
#define SAFETY_SAMPLE_COUNT 5

/** 无效距离的约定值 */
#define SAFETY_INVALID_DISTANCE (-1)

/** 原地转判定死区（RPM）：|左+右|/2 小于该值视为原地转 */
#define SAFETY_PIVOT_DEADBAND_RPM 1.0f

typedef struct
{
    float    scale;      ///< 0..1，当前速度比例
    int16_t  distanceCm; ///< 滤波后的有效距离（<0 表示无效）
    uint8_t  enabled;    ///< 0 = 该传感器未启用（未安装），完全放行
    uint8_t  everValid;  ///< 上电后是否测到过有效值
    uint8_t  fault;      ///< 1 = 工作过之后丢数据（fail-safe）
    int16_t  offsetCm;   ///< 安装偏移（传感器内缩于车身外沿时为正）
    uint16_t stopCm;     ///< 停止距离
    uint16_t slowCm;     ///< 开始减速距离

    int16_t samples[SAFETY_SAMPLE_COUNT];
    uint8_t count; ///< 已填充样本数（0..SAFETY_SAMPLE_COUNT）
    uint8_t index; ///< 下一个写入位置
} SafetyChannel;

/**
 * @brief 初始化一路安全通道
 * @param enabled 该传感器是否启用（未安装传 false）
 * @param offsetCm 安装偏移，传感器内缩为正
 * @param stopCm 停止距离（cm）
 * @param slowCm 开始减速距离（cm），必须 > stopCm
 */
void safety_channel_init(SafetyChannel* ch, bool enabled, int16_t offsetCm,
                         uint16_t stopCm, uint16_t slowCm);

/**
 * @brief 送入一次原始测距结果
 * @param rawDistanceCm 原始距离（cm）；无效传 SAFETY_INVALID_DISTANCE
 *
 * 内部做 5 点中值滤波；失败判据见设计文档第 5 节。
 */
void safety_channel_update(SafetyChannel* ch, int16_t rawDistanceCm);

/**
 * @brief 按运动方向施加限速
 *
 * - 前进（左右平均 > 死区）：使用前向 scale
 * - 倒车（左右平均 < -死区）：使用后向 scale
 * - 原地转：不缩放
 *
 * @return 实际使用的 scale（1.0 表示未限制）
 */
float safety_governor_apply(const SafetyChannel* front, const SafetyChannel* rear,
                            float* leftRpm, float* rightRpm);

#endif /* SERVICES_SAFETY_INC_SAFETY_SERVICE_H_ */
