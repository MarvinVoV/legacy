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

/** Validate and queue a typed request from CtrlTask. */
void SonarTask_Submit(const uint8_t *payload, uint16_t len);

/** Atomic published copies; no reader observes a partially updated window. */
void Sonar_GetSafetySnapshot(SafetyChannel* front, SafetyChannel* rear);

#endif /* TASKS_SONARTASK_INC_SONAR_TASK_H_ */
