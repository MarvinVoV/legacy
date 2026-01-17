/*
 * motor_task.h
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */

#ifndef TASKS_MOTORTASK_INC_MOTOR_TASK_H_
#define TASKS_MOTORTASK_INC_MOTOR_TASK_H_

#include "motor_service.h"

void MotorTask_init(MotorService* motorService);
void MotorTask_doTask(void* arg);

#endif /* TASKS_MOTORTASK_INC_MOTOR_TASK_H_ */
