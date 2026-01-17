/*
 * pic_controller.c - 增量式PID控制器实现
 *
 * 1. 完全遵循增量式PID公式: Δu = Kp·(e[k]-e[k-1]) + Ki·e[k]·Δt + Kd·[(e[k]-e[k-1]) - (e[k-1]-e[k-2])]/Δt
 *
 *  Created on: Mar 16, 2025
 *      Author: marvin
 */


#include <math.h>
#include <pid_controller.h>
#include "log_manager.h"

void PIDController_Init(PID_Controller* pid, const PID_Params* params)
{
    if (!pid || !params) return;

    // 参数安全初始化
    pid->params.kp                 = fmaxf(params->kp, 0.0f);
    pid->params.ki                 = fmaxf(params->ki, 0.0f);
    pid->params.kd                 = fmaxf(params->kd, 0.0f);
    pid->params.output_max         = fmaxf(params->output_max, 0.0f);
    pid->params.dead_zone          = fmaxf(params->dead_zone, 0.0f);
    pid->params.integral_threshold = (params->integral_threshold > 0.0f) ? params->integral_threshold : 0.01f;

    // 初始化状态
    memset(&pid->state, 0, sizeof(PID_State));
    pid->state.first_run        = true;
    pid->state.direction_stable = true;
    pid->state.stable_threshold = 0.1f;
}


float PID_Calculate(PID_Controller* pid, float setpoint, float measurement, float delta_time, uint8_t index)
{
    // 参数检查
    if (pid == NULL)
    {
        return 0.0f;
    }
    if (delta_time <= 0.0f)
    {
        return pid->state.prev_output;
    }

    // 计算当前误差
    float error = setpoint - measurement;

    // 动态死区
    const float dynamic_dead_zone = pid->params.dead_zone * fmaxf(fabsf(setpoint), 0.1f);
    if (fabsf(error) < dynamic_dead_zone)
    {
        error = 0.0f;
    }

    // 首次运行初始化
    if (pid->state.first_run)
    {
        pid->state.prev_error       = error;
        pid->state.prev_prev_error  = error;
        pid->state.prev_measurement = measurement;
        pid->state.prev_output      = 0.0f;
        pid->state.filtered_deriv   = 0.0f;
        pid->state.first_run        = false;
        return 0.0f; // 首次运行返回0
    }

    // 1.比例项增量: Kp * (e[k] - e[k-1])
    const float delta_error = error - pid->state.prev_error;
    const float p_term      = pid->params.kp * delta_error;

    // 2. 积分项增量: Ki * e[k] * Δt
    float i_term = 0.0f;
    // 仅在误差大于阈值时进行积分(避免小误差积分导致振荡)
    if (fabsf(error) > pid->params.integral_threshold ||
        (fabsf(error) > 0.001f && fabsf(pid->state.prev_output) < pid->params.output_max * 0.98f))
    {
        i_term = pid->params.ki * error * delta_time;
        // 抗饱和: 当输出接近饱和且误差同向时，跳过本次积分增量
        if ((fabsf(pid->state.prev_output) >= pid->params.output_max * 0.95f))
        {
            if ((pid->state.prev_output > 0.0f && error > 0.0f) ||
                (pid->state.prev_output < 0.0f && error < 0.0f))
            {
                i_term = 0.0f;
            }
        }
    }


    // 3. 微分项增量: Kd * [(e[k]-e[k-1]) - (e[k-1]-e[k-2])]/Δt
    float d_term = 0.0f;
    if (pid->params.kd > 0.0f)
    {
        // 计算误差的二阶差分(加速度)
        const float prev_delta_error  = pid->state.prev_error - pid->state.prev_prev_error;
        const float second_derivative = (delta_error - prev_delta_error) / delta_time;

        // 低通滤波(抑制微分噪声)
        pid->state.filtered_deriv = 0.85f * pid->state.filtered_deriv + 0.15f * second_derivative;
        d_term                    = pid->params.kd * pid->state.filtered_deriv;
    }

    // 4. 计算总输出增量
    const float delta_output = p_term + i_term + d_term;

    // 5. 计算新输出(位置式输出)
    float output = pid->state.prev_output + delta_output;

    // 6. 输出限幅
    output = CLAMP(output, -pid->params.output_max, pid->params.output_max);


    // 检查是否应该保持当前方向
    if (pid->state.direction_stable)
    {
        // 如果设定点为正且系统稳定，避免不必要的负输出
        if (setpoint > pid->state.stable_threshold && output < -0.05f && error > 0.0f)
        {
            // 过度校正检测：设定点为正但输出为负且误差为正
            output = fmaxf(output, -0.05f); // 限制最小负输出为-5%
        }
        // 如果设定点为负且系统稳定，避免不必要的正输出
        else if (setpoint < -pid->state.stable_threshold &&
                 output > 0.05f &&
                 error < 0.0f)
        {
            output = fminf(output, 0.05f); // 限制最大正输出为5%
        }
    }

    // 检测系统是否稳定在期望方向
    if (fabsf(error) < pid->state.stable_threshold * 0.5f && fabsf(delta_error) < 0.01f)
    {
        pid->state.direction_stable = true;
    }
    else if (fabsf(error) > pid->state.stable_threshold * 2.0f)
    {
        pid->state.direction_stable = false;
    }

    // 检测启动阶段（静止到运动）
    if (pid->state.prev_output == 0.0f &&
        fabsf(pid->state.prev_measurement) < 0.02f &&
        fabsf(error) > 0.1f)
    {
        // 启动补偿：提供额外推力
        const float startup_boost = 0.12f; // 12%启动推力
        if (error > 0.0f)
        {
            output = fmaxf(output, startup_boost);
        }
        else
        {
            output = fminf(output, -startup_boost);
        }
    }

    // 7. 更新状态(顺序很重要!)
    pid->state.prev_prev_error  = pid->state.prev_error;
    pid->state.prev_error       = error;
    pid->state.prev_measurement = measurement;
    pid->state.prev_output      = output;

    // if (index == 1)
    // {
    //     // 调试日志 - 精简版，仅保留核心参数
    //     LOG_INFO(LOG_MODULE_SYSTEM,
    //              "sp=%.2f,mm=%.2f,e=%.2f,kp=%.2f,ki=%.2f,kd=%.2f,pe=%.2f,ppe=%.2f,op=%.2f,dt=%.2f",
    //              setpoint, measurement, (setpoint - measurement), pid->params.kp, pid->params.ki, pid->params.kd,
    //              pid->state.prev_error, pid->state.prev_prev_error, output, delta_time);
    // }

    return output;
}

void PID_Reset(PID_Controller* pid)
{
    if (!pid) return;
    memset(&pid->state, 0, sizeof(PID_State));
    pid->state.first_run        = true;
    pid->state.direction_stable = true;
    pid->state.stable_threshold = 0.1f;
}
