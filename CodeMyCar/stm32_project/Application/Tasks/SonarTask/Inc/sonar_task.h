/*
 * sonar_task.h
 *
 * 超声波测距任务：错开触发前后传感器、更新安全通道、上报距离。
 */

#ifndef TASKS_SONARTASK_INC_SONAR_TASK_H_
#define TASKS_SONARTASK_INC_SONAR_TASK_H_

#include "safety_service.h"

/** 创建并启动 SonarTask（需先调用 SonarHw_Init） */
void SonarTask_Init(void);

/** 任务体（由 osThreadNew 调用） */
void SonarTask_doTask(void* arg);

/**
 * @brief 取前/后安全通道（运动层限速使用）
 *
 * 初始化前返回的通道 enabled=0、scale=1.0，即完全放行。
 */
void Sonar_GetSafety(const SafetyChannel** front, const SafetyChannel** rear);

#endif /* TASKS_SONARTASK_INC_SONAR_TASK_H_ */
