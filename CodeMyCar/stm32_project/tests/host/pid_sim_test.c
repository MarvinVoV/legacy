/*
 * pid_sim_test.c — PID + 前馈 + PWM 滤波的 host 仿真（一阶电机模型）
 *
 * 目的：不接电机，观察当前参数在不同控制周期下的阶跃响应。
 * 注意：一阶模型不含真实摩擦/负载，结果只用于判断趋势和周期一致性；
 *       最终参数需用真机阶跃响应标定。
 */

#include <math.h>
#include <stdio.h>

#include "pid_controller.h"

typedef struct
{
    float v;    /* 归一化转速 0..1 */
    float tau;  /* 时间常数 (s) */
    float gain; /* duty 1.0 -> v = gain */
} Plant;

/* 与 motor_driver.c::calculate_feedforward 一致（简化 copy） */
static float feedforward(float target_rpm, float actual_rpm)
{
    const float dir = (target_rpm >= 0.0f) ? 1.0f : -1.0f;
    const float at  = fabsf(target_rpm);
    const float ar  = fabsf(actual_rpm);

    float base = at * 0.0028f * dir;
    if (at < 20.0f) base *= 0.7f + 0.3f * (at / 20.0f);
    if (at <= 15.0f && ar < 5.0f) base *= 0.7f * fmaxf(0.3f, at / 15.0f) + 0.3f;

    float friction = 0.0f;
    if (ar < 5.0f && at > 0.1f)
    {
        const float tf = fminf(at / 50.0f, 1.0f);
        const float sf = 0.01f + 0.02f * tf;
        friction = sf * (1.0f - fminf(ar / 5.0f, 1.0f)) * dir;
    }
    else if (at > 0.1f)
    {
        friction = (0.005f + 0.005f * (ar / 50.0f)) * dir;
    }

    float ff = base + friction;
    if (ar < 5.0f && at > 10.0f)
    {
        const float ms = 0.08f + 0.02f * (at / 50.0f);
        ff = CLAMP(ff, -ms, ms);
    }
    return CLAMP(ff, -1.0f, 1.0f);
}

static void plant_step(Plant* p, float u, float dt)
{
    const int sub = 10;
    const float h = dt / sub;
    for (int i = 0; i < sub; i++)
    {
        p->v += ((u * p->gain - p->v) / p->tau) * h;
        if (p->v < 0.0f) p->v = 0.0f;
    }
}

static void run_case(float dt, float setpoint_rpm, float max_rpm)
{
    PID_Params params = {
        .kp = 0.68f, .ki = 0.9f, .kd = 0.09f,
        .output_max = 1.0f, .integral_threshold = 0.005f, .dead_zone = 0.002f,
    };
    PID_Controller pid;
    PIDController_Init(&pid, &params);

    Plant plant = {.v = 0.0f, .tau = 0.15f, .gain = 1.0f};
    const float target = setpoint_rpm / max_rpm;
    const float duration = 3.0f;
    const int steps = (int)(duration / dt);

    float pwm_state = 0.0f, pwm_alpha = 0.7f;
    float vmax = 0.0f, t_rise = -1.0f, t_settle = -1.0f;
    int settled = 0;

    for (int k = 0; k < steps; k++)
    {
        const float t = k * dt;
        float out = PID_Calculate(&pid, target, plant.v, dt, 0);
        out += feedforward(setpoint_rpm, plant.v * max_rpm);
        pwm_state = pwm_alpha * out + (1.0f - pwm_alpha) * pwm_state;
        const float u = CLAMP(pwm_state, -1.0f, 1.0f);
        plant_step(&plant, u, dt);

        if (plant.v > vmax) vmax = plant.v;
        if (t_rise < 0.0f && plant.v >= 0.9f * target) t_rise = t;
        if (t > 0.2f && fabsf(plant.v - target) <= 0.02f * target)
        {
            if (!settled) { settled = 1; t_settle = t; }
        }
        else if (t > 0.2f)
        {
            settled = 0;
            t_settle = -1.0f;
        }
    }

    printf("dt=%.3fs sp=%.0fRPM: rise=%s%.3fs settle=%s%.3fs overshoot=%.1f%% "
           "final_err=%.2fRPM out=%.2f v=%.3f\n",
           dt, setpoint_rpm,
           t_rise < 0 ? "none/" : "", t_rise,
           t_settle < 0 ? "none/" : "", t_settle,
           (vmax - target) / target * 100.0f,
           (target - plant.v) * max_rpm, pwm_state, plant.v);
}

int main(void)
{
    printf("PID sim (plant: gain=1, tau=0.15s, friction/feedforward copied)\n");
    run_case(0.005f, 100.0f, 300.0f);
    run_case(0.030f, 100.0f, 300.0f);
    run_case(0.005f, 300.0f, 300.0f);
    run_case(0.030f, 300.0f, 300.0f);
    return 0;
}
