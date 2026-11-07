/*
 * heading_control.h
 *
 * 航向闭环：把 IMU 的 yaw / 角速度反馈注入转向曲率
 *
 *   直行：e = ψ_ref − ψ          → κ_corr = Kp·e + Ki∫e
 *   转弯：e = v·κ_ff − ψ̇        → κ_corr = Kp·e + Ki∫e
 *
 * 输出限幅、抗积分饱和、死区（吸收约 4° 机械回差）。
 */

#ifndef SERVICES_CTRL_INC_HEADING_CONTROL_H_
#define SERVICES_CTRL_INC_HEADING_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool active;         /* 是否有有效 IMU 数据 */
    bool straightMode;   /* 上一次使用的模式 */
    float psiRefDeg;     /* 直行目标航向 */
    float integral;      /* 直行角度积分 (1/m) */
    float rateIntegral;  /* 转弯角速度积分 */
    uint32_t lastTickMs;

    /* 参数（直行角度环） */
    float kp;            /* (1/m)/deg */
    float ki;            /* (1/m)/(deg·s) */
    float deadbandDeg;

    /* 参数（转弯曲率/角速度环） */
    float kpRate;
    float kiRate;
    float deadbandK;     /* 1/m */

    float maxCorr;       /* 1/m */

    /* 诊断 */
    float lastErrorDeg;
    float lastCorr;
} HeadingControl;

/** 初始化参数（默认值见实现）。传入 0 表示用默认。 */
void HeadingControl_Init(HeadingControl* hc, float kp, float ki);

/** 重新锁定直行航向（进入直行时调用）。 */
void HeadingControl_Reset(HeadingControl* hc, float yawDeg);

/** 关闭闭环并清积分（IMU 失效、急停时调用）。 */
void HeadingControl_Disable(HeadingControl* hc);

/**
 * 计算修正后的曲率。
 *
 * @param hc        控制器
 * @param yawDeg    当前航向（连续角）
 * @param yawRateDps 当前偏航角速度
 * @param vMps      实测线速度（m/s）
 * @param kappaFf   前馈曲率（命令值）
 * @param straight  true=直行（航向保持）；false=转弯（角速度跟随）
 * @return 修正后的曲率（含前馈，已限幅）
 */
float HeadingControl_Apply(HeadingControl* hc, float yawDeg, float yawRateDps,
                           float vMps, float kappaFf, bool straight);

#endif /* SERVICES_CTRL_INC_HEADING_CONTROL_H_ */
