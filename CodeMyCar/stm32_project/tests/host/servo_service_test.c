#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "servo_service.h"

static uint32_t tick_ms;
static float last_hw_angle;

uint32_t HAL_GetTick(void) { return tick_ms; }
osMutexId_t osMutexNew(const void* attr) { (void)attr; return (void*)1; }
osStatus_t osMutexAcquire(osMutexId_t mutex, uint32_t timeout)
{ (void)mutex; (void)timeout; return osOK; }
osStatus_t osMutexRelease(osMutexId_t mutex) { (void)mutex; return osOK; }
int ServoDriver_Init(ServoDriver* driver) { return driver ? 0 : -1; }
int ServoDriver_setAngle(ServoDriver* driver, float angle)
{ (void)driver; last_hw_angle = angle; return 0; }
int ServoDriver_emergencyStop(const ServoDriver* driver) { (void)driver; return 0; }
void ServoTask_init(void* instance) { (void)instance; }

int main(void)
{
    ServoDriver driver = {0};
    SteerConfig cfg = {
        .minAngleDeg = 0.0f, .maxAngleDeg = 180.0f, .deadZoneDeg = 0.01f,
        .maxSpeedDegPerSec = 90.0f, .accelerationDegPerSec2 = 180.0f,
        .updateIntervalMs = 20,
    };
    SteerInstance instance = {0};
    assert(SteerService_init(&instance, &cfg, &driver) == 0);
    assert(fabsf(instance.smoothCtrl.accelerationDegPerMs2 - 0.00018f) < 1e-8f);

    assert(SteerService_setAngleSmoothly(&instance, 90.0f) == 0);
    assert(fabsf(instance.state.targetAngleDeg - 90.0f) < 1e-6f);
    tick_ms += 20;
    assert(SteerService_update(&instance) == 0);
    const float speed = instance.smoothCtrl.currentSpeedDegPerMs;
    const float angle = instance.state.commandedAngleDeg;
    assert(speed > 0.0f && angle > 0.0f);

    assert(SteerService_setAngleSmoothly(&instance, 90.0f) == 0);
    assert(fabsf(instance.smoothCtrl.currentSpeedDegPerMs - speed) < 1e-9f);
    assert(fabsf(instance.state.commandedAngleDeg - angle) < 1e-9f);

    for (int i = 0; i < 200; ++i)
    {
        tick_ms += 20;
        assert(SteerService_update(&instance) == 0);
    }
    assert(instance.state.commandedAngleDeg <= 90.0f);
    assert(last_hw_angle <= 90.0f);
    puts("servo_service: all tests passed");
    return 0;
}
