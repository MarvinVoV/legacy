/*
 * sonar_hw.h
 *
 * 超声波硬件初始化（手写，不依赖 CubeMX 生成的 Core/ 文件）。
 */

#ifndef SYSTEM_INIT_INC_SONAR_HW_H_
#define SYSTEM_INIT_INC_SONAR_HW_H_

#include "ultrasonic.h"

/**
 * @brief 初始化 Trig/Echo GPIO、EXTI、TIM7 时基和 DWT
 *
 * 必须在 osKernelStart 之前调用（只配置外设，不阻塞）。
 */
void SonarHw_Init(void);

/** 前/后传感器实例（任务使用） */
UltrasonicSensor* SonarHw_Front(void);
UltrasonicSensor* SonarHw_Rear(void);

#endif /* SYSTEM_INIT_INC_SONAR_HW_H_ */
