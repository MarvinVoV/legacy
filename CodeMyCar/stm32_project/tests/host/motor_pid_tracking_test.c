/* Independent first-order motor plant: feedforward + PID rejects a small
 * constant load. No knowledge of controller state or integral implementation. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "pid_controller.h"
#include "motor_pid_defaults.h"

static void run(float target, float load_sign)
{
    PID_Controller pid;
    const PID_Params params = MOTOR_PID_DEFAULT_PARAMS;
    PIDController_Init(&pid, &params);
    float speed = target + load_sign * 0.0006f;
    double mean = 0, squared = 0;
    for (int k = 0; k < 4000; ++k) {
        const float load = load_sign * (k < 1000 ? 0.0006f : 0.0009f);
        const float effort = target + PID_Calculate(&pid, target, speed, .005f, 0);
        /* 0.1% PWM quantization, comparable to a 1000-count timer. */
        const float pwm = roundf(effort * 1000.f) / 1000.f;
        speed += .005f / .15f * (pwm + load - speed);
        assert(isfinite(speed) && fabsf(speed) < fabsf(target) + .01f);
        if (k >= 3000) {
            mean += speed - target;
            squared += (speed-target)*(speed-target);
        }
    }
    printf("target=%+.1f RPM load=%+.2f RPM tail mean=%+.4f RPM rms=%.4f RPM\n",
           target*300, load_sign*.27f, mean/1000*300, sqrt(squared/1000)*300);
    fflush(stdout);
#if CMCAR_MOTOR_PID_FINE_INTEGRATION
    assert(fabs(mean / 1000) < .00025); /* < .075 RPM mean tracking bias */
    assert(sqrt(squared / 1000) < .0004);
#else
    /* Characterize the known baseline floor; do not call this precision acceptance. */
    assert(fabs(mean / 1000) > .0007 && fabs(mean / 1000) < .0011);
#endif
}
int main(void)
{
    for (int magnitude=1; magnitude<=3; magnitude+=2)
        for (int t=-1; t<=1; t+=2)
            for (int l=-1; l<=1; l+=2) run(t*magnitude*.1f, (float)l);
    PID_Controller idle;
    const PID_Params params = MOTOR_PID_DEFAULT_PARAMS;
    PIDController_Init(&idle, &params);
    for (int i=0; i<4000; ++i)
        assert(PID_Calculate(&idle, 0, 0, .005f, 0) == 0);
    puts(CMCAR_MOTOR_PID_FINE_INTEGRATION ? "experimental motor PID plant tracking: PASS" : "baseline motor PID known tracking floor: reproduced");
}
