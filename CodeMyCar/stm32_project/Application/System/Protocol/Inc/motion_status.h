#ifndef MOTION_STATUS_H
#define MOTION_STATUS_H

/**
 * @file motion_status.h
 * @brief 运动状态上行帧（36 字节，小端），STM32 → ESP32 → pyserver
 *
 * 字段单位全部为设备原始单位（mm / 0.001° / mm/s），bridge 不做换算。
 */

#include <stdint.h>

#define MOTION_STATUS_SIZE 36

/* state 取值 */
#define MOTION_STATE_IDLE    0
#define MOTION_STATE_RUNNING 1
#define MOTION_STATE_DONE    2
#define MOTION_STATE_ABORTED 3
#define MOTION_STATE_ERROR   4

/* error 取值 */
#define MOTION_ERR_NONE     0
#define MOTION_ERR_MODE     1
#define MOTION_ERR_PARAM    2
#define MOTION_ERR_GEOMETRY 3
#define MOTION_ERR_BUSY     4
#define MOTION_ERR_INTERNAL 5
#define MOTION_ERR_STALL    6
#define MOTION_ERR_TASK_STALE 7
#define MOTION_ERR_ENCODER  8
#define MOTION_ERR_IMU      9

#pragma pack(push, 1)
typedef struct
{
    uint8_t  seq;            /* 当前/最近一次 goal 的 seq */
    uint8_t  state;          /* @see MOTION_STATE_* */
    uint8_t  error;          /* @see MOTION_ERR_* */
    uint8_t  flags;          /* bit0 stale, bit1..3 mode, bit4 fault, bit5 lease expired */
    int32_t  xMm;            /* 位姿 x（前正） */
    int32_t  yMm;            /* 位姿 y（设备数学坐标：左正） */
    int32_t  thetaMdeg;      /* 航向（CCW 正，0.001°） */
    int32_t  odometerMm;     /* 启动以来带方向的累计轮程；倒车减少，非累计总路程 */
    int32_t  goalDistanceMm; /* 本次 goal 已行进距离（带方向） */
    int32_t  goalTurnMdeg;   /* 本次 goal 已转角度（右正，未归一化） */
    uint32_t goalDurationMs; /* 本次 goal 已用时 */
    int16_t  leftMmps;       /* 左轮线速度 */
    int16_t  rightMmps;      /* 右轮线速度 */
} motion_status_t;
#pragma pack(pop)

#define MOTION_STATUS_FLAG_STALE         (1u << 0)
#define MOTION_STATUS_FLAG_MODE_SHIFT    1u
#define MOTION_STATUS_FLAG_MODE_MASK     (0x07u << MOTION_STATUS_FLAG_MODE_SHIFT)
#define MOTION_STATUS_FLAG_FAULT         (1u << 4)
#define MOTION_STATUS_FLAG_LEASE_EXPIRED (1u << 5)
#define MOTION_STATUS_FLAG_GOAL_IMU      (1u << 6)

_Static_assert(sizeof(motion_status_t) == MOTION_STATUS_SIZE,
               "motion_status_t wire layout must stay 36 bytes");

/**
 * @brief 打包为 36 字节小端
 * @return 0 成功，-1 参数错误
 */
int motion_status_pack(const motion_status_t* s, uint8_t out[MOTION_STATUS_SIZE]);

/**
 * @brief 从 36 字节小端解包
 * @return 0 成功，-1 参数错误或长度不足
 */
int motion_status_unpack(const uint8_t* in, uint8_t len, motion_status_t* s);

#endif /* MOTION_STATUS_H */
