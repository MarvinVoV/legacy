/*
 * imu_service.h
 *
 * 航向估计服务（基于 Hillcrest 官方 SH-2 协议栈）：
 *   BNO085 四元数 → yaw；陀螺 → 角速度；静止零偏标定
 */

#ifndef SERVICES_IMU_INC_IMU_SERVICE_H_
#define SERVICES_IMU_INC_IMU_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "stm32h7xx_hal.h"

/* 跨任务只读快照：保证 yaw / yawRate / status / 时间戳来自同一版本 */
typedef struct
{
    bool valid;
    float yawDeg;
    float yawRateDps;
    uint8_t status;
    uint32_t lastUpdateMs;
} ImuSnapshot;

typedef struct
{
    volatile uint32_t seq; /* seqlock：奇数表示写入中 */

    bool inited;
    bool yawValid;

    /* 最新原始数据 */
    float quatW, quatX, quatY, quatZ;
    float gyroX, gyroY, gyroZ; /* rad/s */
    float accelX, accelY, accelZ; /* m/s^2 */

    /* 航向 */
    float yawDeg;      /* 连续角（不回绕） */
    float yawRateDps;  /* 零偏修正后的角速度 */
    float biasDps;     /* 静止标定得到的陀螺零偏 */
    uint8_t status;

    float lastRawYawDeg;
    bool lastRawValid;

    /* 零偏标定 */
    bool calibrating;
    uint32_t calibCount;
    uint32_t calibLastGyroCount; /* 只在新陀螺样本上累计 */
    double calibSumDps;

    /* 诊断 */
    uint32_t grvCount;
    uint32_t gyroCount;
    uint32_t accelCount;
    bool biasValid; /* 零偏标定完成 */
    uint32_t lastUpdateMs;
} ImuService;

/** 打开 SH-2 会话并使能报告。失败返回非 0。 */
int ImuService_Init(ImuService* s, I2C_HandleTypeDef* i2c);

/** 读取一致快照（seqlock，无需加锁）。返回 false 表示读取期间被打断。 */
bool ImuService_GetSnapshot(ImuService* s, ImuSnapshot* out);

/** 100 Hz 调用：service 协议栈 + 更新 yaw / yawRate / bias。 */
void ImuService_Update(ImuService* s);

/** 关闭会话（供任务在超时后重新初始化）。 */
void ImuService_Close(ImuService* s);

/** 把航向基准重置为当前朝向。 */
void ImuService_ResetYawReference(ImuService* s);

#endif /* SERVICES_IMU_INC_IMU_SERVICE_H_ */
