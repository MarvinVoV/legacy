#pragma once
#include "cmsis_os2.h"
#define QUEUE_TYPE_LOG 1
void QueueManager_registerHandler(int,osMessageQueueId_t);
