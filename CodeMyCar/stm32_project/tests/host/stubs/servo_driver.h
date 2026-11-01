#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } HAL_ServoConfig;
typedef struct { HAL_ServoConfig* hw; } ServoDriver;
int ServoDriver_Init(ServoDriver* driver);
int ServoDriver_setAngle(ServoDriver* driver, float angleDeg);
int ServoDriver_emergencyStop(const ServoDriver* driver);
