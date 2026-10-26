#pragma once

/**
 * @file motion_goal.h
 * @brief 目标型运动控制（距离 + 转角），纯 C、可 host 测试
 *
 * 语义：走 distanceMm 的圆弧，同时航向变化 turnDdeg（右转为正，相对增量）。
 * 航向累加器不做归一化，因此 270°/720° 都能正确执行。
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float wheelbaseMm;
    float trackWidthMm;
    float wheelRadiusMm;
    float maxSteerDeg;
    float maxRpm;
    float maxAngularRadps;
    float decelMmps2;
} MotionGoalConfig;

typedef struct
{
    int16_t  turnDdeg;   /* 航向变化，右正，±3600 */
    int16_t  distanceMm; /* 路程，前进正，±5000 */
    uint16_t speedMmps;  /* 路径速度，1..600 */
} MotionGoalTarget;

typedef struct
{
    MotionGoalTarget target;
    uint8_t          seq;  /* 帧 seq，随状态上报回传 */
    float            headingTravelDeg; /* 右正，未归一化 */
    float            distanceTravelMm; /* 带行进方向 */
    uint32_t         startMs;
    uint32_t         finishMs;
    uint32_t         endgameStartMs;
    bool             inEndgame;
    bool             active;
    bool             finished;
    bool             aborted;
    uint8_t          error; /* 0 ok, 1 param, 3 geometry, 5 internal */
} MotionGoalState;

typedef struct
{
    float   leftRpm;
    float   rightRpm;
    float   steerAngleDeg; /* 右正 */
    bool    pivot;         /* true: 舵机归中，差速原地转 */
    bool    finished;
    uint8_t error;
} MotionGoalOutput;

/**
 * @brief 初始化并校验目标
 * @return 0 成功；1 参数非法；3 曲率半径不足
 */
int motion_goal_start(MotionGoalState* s, const MotionGoalTarget* t,
                      const MotionGoalConfig* cfg, uint32_t nowMs);

/**
 * @brief 用本周期左右轮行程增量推进累加器（单位 mm）
 */
void motion_goal_accumulate(MotionGoalState* s, float deltaLeftMm,
                            float deltaRightMm, const MotionGoalConfig* cfg);

/**
 * @brief 计算本周期输出
 */
MotionGoalOutput motion_goal_step(MotionGoalState* s, const MotionGoalConfig* cfg,
                                  uint32_t nowMs);
