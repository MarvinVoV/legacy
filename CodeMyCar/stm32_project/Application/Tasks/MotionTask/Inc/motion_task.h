/*
 * motion_task.h
 *
 *  Created on: Apr 23, 2025
 *      Author: marvin
 */

#ifndef TASKS_MOTIONTASK_INC_MOTION_TASK_H_
#define TASKS_MOTIONTASK_INC_MOTION_TASK_H_

#include "motion_service.h"

void MotionTask_init(MotionContext* motion_context);

void MotionTask_doTask(void *arg);

#endif /* TASKS_MOTIONTASK_INC_MOTION_TASK_H_ */
