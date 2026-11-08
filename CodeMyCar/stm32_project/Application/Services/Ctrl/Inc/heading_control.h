/*
 * heading_control.h
 *
 * 航向闭环：把 IMU 的 yaw / 角速度反馈注入转向曲率
 *
 *   ψ̇=-v·κ；直行 e=sign(v)·(ψ-ψ_ref)，转弯 e=κ_ff-(-ψ̇/v)。
 *
 * 输出限幅、抗积分饱和、小角度死区；直行GOAL另含测量微分阻尼。
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
    int8_t motionDirection;
    int8_t curveDirection;

    /* 参数（直行角度环） */
    float kp;            /* legacy DRIVE angle gain, (1/m)/deg */
    float kpGoal;        /* fixed-reference GOAL angle gain */
    float ki;            /* (1/m)/(deg·s) */
    float deadbandDeg;
    float kd; /* GOAL damping, (1/m)/(deg/s), filtered quaternion derivative */
    float previousYawDeg;
    float filteredYawRateDps;
    bool yawRateTracking;

    /* 参数（转弯曲率/角速度环） */
    float kpRate;
    float kiRate;
    float deadbandK;     /* 1/m */

    float maxCorr;       /* 修正量限幅 (1/m) */
    float maxStraightCorr; /* 直行需能抵消固定转向偏置，独立于弯道反馈限幅 */
    float maxKappa;      /* 最终曲率限幅 (1/m)，与 STEERING_KAPPA_MAX_1PM 一致 */

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

/* 距离型直行 GOAL：以事务开始时的航向为固定参考，无残差曲率前馈。 */
float HeadingControl_HoldHeading(HeadingControl* hc, float yawDeg, float yawRateDps,
                                float vMps, float referenceDeg);

#endif /* SERVICES_CTRL_INC_HEADING_CONTROL_H_ */
