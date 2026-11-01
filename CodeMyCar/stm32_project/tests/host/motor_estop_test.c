/*
 * motor_estop_test.c — SAFE-01/02 emergency-stop regression test.
 *
 * This is a host test for MotorDriver_EmergencyStop.  Hardware calls are
 * replaced with small stubs so the state-reset contract can be verified
 * without an STM32 target.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "motor_driver.h"

static uint32_t s_tick = 1234u;
static unsigned s_hal_stop_calls;

uint32_t HAL_GetTick(void)
{
    return s_tick;
}

HAL_StatusTypeDef HAL_Motor_Init(HAL_MotorConfig* config)
{
    (void)config;
    return HAL_OK;
}

void HAL_Motor_SetDuty(HAL_MotorConfig* config, float duty)
{
    config->state.currentDuty = (int32_t)duty;
}

void HAL_Motor_EmergencyStop(HAL_MotorConfig* config)
{
    s_hal_stop_calls++;
    config->state.currentDuty = 0;
    config->state.direction = MOTOR_BRAKE_STOP;
    /* The encoder counter is intentionally preserved by the host HAL stub. */
}

void HAL_Motor_Brake(HAL_MotorConfig* config)
{
    config->state.currentDuty = 0;
    config->state.direction = MOTOR_BRAKE_STOP;
}

uint32_t HAL_Motor_ReadRawEncoder(HAL_MotorConfig* config)
{
    return config->encoder.tim->counter;
}

uint32_t HAL_Motor_GetEncoderMaxValue(const HAL_MotorConfig* config)
{
    (void)config;
    return 0xFFFFFFFFu;
}

static int near_zero(float value)
{
    return fabsf(value) < 0.00001f;
}

int main(void)
{
    float filter_buffer[4] = {11.0f, 12.0f, 13.0f, 14.0f};
    TIM_HandleTypeDef encoder = {.counter = 4321u};
    TIM_HandleTypeDef pwm = {0};
    HAL_MotorConfig hal = {
        .pwm = {.tim = &pwm, .ch = 1u},
        .encoder = {.tim = &encoder},
        .state = {
            .currentDuty = 37,
            .direction = MOTOR_DIR_FORWARD,
            .encoderTicks = 4321,
        },
    };
    const MotorSpec spec = {
        .encoderPPR = 13,
        .gearRatio = 30,
        .wheelRadiusMM = 32.5f,
        .maxRPM = 300.0f,
        .feedforwardScale = 1.4f,
    };
    MotorDriver driver;
    memset(&driver, 0, sizeof(driver));
    driver.halCfg = &hal;
    driver.spec = &spec;
    driver.control.mode = MOTOR_DRIVER_MODE_CLOSED_LOOP;
    driver.control.direction = MOTOR_DIR_FORWARD;
    driver.control.targetRPM = -30.0f;
    driver.control.rampedTargetRPM = 24.0f;
    driver.control.pidCtrl.state.prev_error = 0.3f;
    driver.control.pidCtrl.state.prev_output = 0.4f;
    driver.control.pidCtrl.state.first_run = false;
    driver.filter.windowSize = 4;
    driver.filter.filterBuffer = filter_buffer;
    driver.filter.bufferIndex = 3;
    driver.filter.sampleCount = 4;
    driver.filter.lpf_state = 22.0f;
    driver.filter.pwm_filter_state = 0.35f;
    driver.state.velocity.rpm = 24.0f;
    driver.state.velocity.radps = 2.5f;
    driver.state.velocity.mps = 0.08f;
    driver.state.openLoopDuty = 37.0f;
    driver.state.appliedDuty = 0.37f;
    driver.state.mode = MOTOR_DRIVER_MODE_CLOSED_LOOP;
    driver.state.direction = MOTOR_DIR_FORWARD;

    /* Position is measurement state and must survive a stop. */
    driver.state.position.totalPulses = 9876;
    driver.state.position.revolutions = 6.3307f;
    driver.state.position.lastRawPulses = 4321u;
    driver.state.position.encoderInitialized = true;

    MotorDriver_EmergencyStop(&driver);

    assert(s_hal_stop_calls == 1u);
    assert(driver.control.mode == MOTOR_DRIVER_MODE_STOP);
    assert(driver.state.mode == MOTOR_DRIVER_MODE_STOP);
    assert(driver.control.direction == MOTOR_BRAKE_STOP);
    assert(driver.state.direction == MOTOR_BRAKE_STOP);
    assert(near_zero(driver.control.targetRPM));
    assert(near_zero(driver.control.rampedTargetRPM));
    assert(near_zero(driver.control.pidCtrl.state.prev_error));
    assert(near_zero(driver.control.pidCtrl.state.prev_output));
    assert(driver.control.pidCtrl.state.first_run);
    assert(driver.filter.bufferIndex == 0u);
    assert(driver.filter.sampleCount == 0u);
    assert(near_zero(driver.filter.lpf_state));
    assert(near_zero(driver.filter.pwm_filter_state));
    for (unsigned i = 0; i < driver.filter.windowSize; i++)
    {
        assert(near_zero(driver.filter.filterBuffer[i]));
    }
    assert(near_zero(driver.state.velocity.rpm));
    assert(near_zero(driver.state.velocity.radps));
    assert(near_zero(driver.state.velocity.mps));
    assert(near_zero(driver.state.openLoopDuty));
    assert(near_zero(driver.state.appliedDuty));

    assert(encoder.counter == 4321u);
    assert(hal.state.encoderTicks == 4321);
    assert(driver.state.position.totalPulses == 9876);
    assert(fabsf(driver.state.position.revolutions - 6.3307f) < 0.0001f);
    assert(driver.state.position.lastRawPulses == 4321u);
    assert(driver.state.position.encoderInitialized);

    /* A maintained ESTOP may be requested repeatedly by upper layers. */
    for (unsigned i = 1u; i < 1000u; i++)
    {
        MotorDriver_EmergencyStop(&driver);
    }
    assert(s_hal_stop_calls == 1000u);
    assert(encoder.counter == 4321u);
    assert(hal.state.encoderTicks == 4321);
    assert(driver.state.position.totalPulses == 9876);
    assert(driver.state.position.lastRawPulses == 4321u);
    assert(driver.control.mode == MOTOR_DRIVER_MODE_STOP);
    assert(driver.state.mode == MOTOR_DRIVER_MODE_STOP);
    assert(near_zero(driver.control.targetRPM));
    assert(near_zero(driver.control.rampedTargetRPM));

    printf("motor_estop: all tests passed\n");
    return 0;
}
