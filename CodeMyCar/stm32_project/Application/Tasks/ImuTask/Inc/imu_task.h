/*
 * imu_task.h
 *
 * 100 Hz 姿态任务：读取 BNO085，更新航向估计，并按 10 Hz 输出日志
 */

#ifndef TASKS_IMUTASK_INC_IMU_TASK_H_
#define TASKS_IMUTASK_INC_IMU_TASK_H_

#include "imu_service.h"

/** 创建任务（需在 I2C1 初始化之后调用）。 */
void ImuTask_Init(ImuService* service);

/** 获取服务实例指针（供航向控制器使用）。 */
ImuService* ImuTask_GetService(void);

#endif /* TASKS_IMUTASK_INC_IMU_TASK_H_ */
