/*
 * motor_driver.c
 *
 *  Created on: Mar 15, 2025
 *      Author: marvin
 */


#include "motor_driver.h"
#include "log_manager.h"

/// 最小有效时间间隔 (5ms)
#define MIN_DELTA_TIME_S (0.005f)

/// 默认速度滤波窗口大小
#ifndef DEFAULT_SPEED_FILTER_WINDOW
#define DEFAULT_SPEED_FILTER_WINDOW 5
#endif


//------------------------------------------------------------------------------
// 静态函数声明
//------------------------------------------------------------------------------
/**
 * @brief 将转速从RPM转换为rad/s
 * @param rpm 转速 [RPM]
 * @return float 角速度 [rad/s]
 */
static inline float rpm_to_radps(float rpm);
/**
 * @brief 将角速度从rad/s转换为RPM
 * @param radps 角速度 [rad/s]
 * @return float 转速 [RPM]
 */
static inline float radps_to_rpm(float radps);

/**
 * @brief 将角速度转换为线速度
 * @param radps 角速度 [rad/s]
 * @param wheel_radius_mm 轮半径 [mm]
 * @return float 线速度 [m/s]
 */
static inline float radps_to_mps(float radps, float wheel_radius_mm);


/**
 * @brief 将编码器脉冲数转换为旋转弧度值
 *
 * @param pulses 编码器脉冲数，单位：个（count）
 *              物理意义：四倍频后的累计脉冲数（包含方向信息）
 * @param encoder_ppr 编码器每转脉冲数，单位：脉冲/转（Pulses Per Revolution）
 *                  物理意义：编码器未倍频时每转输出的脉冲数
 * @param gear_ratio 减速比，单位：无因次量（如30:1减速比则传入30.0）
 * @return float 旋转角度值，单位：弧度（rad）
 *
 * @note 转换公式：
 * radians = (pulses × 2π) / (4 × PPR × gearRatio)
 * 四倍频说明：编码器每个脉冲周期产生4个计数（上升沿+下降沿×2通道），所以，4 * PPR 表示电机转一圈产生的总脉冲数（四倍频后）
 * 示例：encoderPPR=13, gearRatio=30, pulses=1560时：
 * (1560 × 6.2832) / (4 × 13 × 30) = 6.2832 rad（正好1转）
 */
static inline float pulses_to_rad(int32_t pulses, uint16_t encoder_ppr, uint16_t gear_ratio);

/**
 * @brief 更新累计转数
 * @param state 状态结构体
 * @param delta_pulses 脉冲增量
 * @param encoder_ppr 编码器每转脉冲数
 * @param gear_ratio 减速比
 */
static void update_revolutions(MotorDriverState* state,
                               int32_t           delta_pulses,
                               uint16_t          encoder_ppr,
                               uint16_t          gear_ratio);

/**
 * @brief 应用速度滤波
 * @param driver 驱动控制器
 * @param raw_speed 原始速度值
 * @return float 滤波后速度
 */
static float apply_speed_filter(MotorDriver* driver, float raw_speed);
static float apply_pwm_filter(MotorDriver* driver, float raw_pwm);
static float calculate_feedforward(float target_rpm, float actual_rpm);
/**
 *  计算脉冲增量
 * @param driver  驱动控制器
 * @param current_raw 当前原始脉冲数
 * @return int32_t 脉冲增量
 */
static int32_t calculate_delta_pulses(MotorDriver* driver, uint32_t current_raw);

//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------

bool MotorDriver_Init(MotorDriver* driver, const PID_Params* initialPid)
{
    // 参数检查
    if (driver == NULL || driver->halCfg == NULL || driver->spec == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor driver initialization failed");
        return false;
    }

    if (driver->spec->encoderPPR == 0 || driver->spec->gearRatio == 0)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid encoderPPR or gearRatio");
        return false;
    }

    PIDController_Init(&driver->control.pidCtrl, initialPid);

    // 初始化滤波系统
    driver->filter.windowSize   = DEFAULT_SPEED_FILTER_WINDOW;
    driver->filter.filterBuffer = (float*)pvPortMalloc(driver->filter.windowSize * sizeof(float));
    if (driver->filter.filterBuffer == NULL)
    {
        LOG_WARN(LOG_MODULE_MOTOR, "Speed filter buffer allocation failed");
        driver->filter.windowSize = 0;
    }
    else
    {
        // 初始化缓冲区
        memset(driver->filter.filterBuffer, 0, driver->filter.windowSize * sizeof(float));
        driver->filter.bufferIndex = 0;
    }
    driver->filter.lpf_state = 0.0f;

    // PWM滤波初始化
    driver->filter.pwm_filter_state = 0.0f;
    driver->filter.pwm_alpha        = 0.7f; // 默认值

    // 状态初始化
    memset(&driver->state, 0, sizeof(MotorDriverState));
    driver->state.mode                 = MOTOR_DRIVER_MODE_STOP;
    driver->state.direction            = MOTOR_DIR_FORWARD;
    driver->state.lastUpdateTick       = HAL_GetTick();
    driver->state.lastControlTick      = HAL_GetTick();
    driver->state.debug.lastLogPrint   = 0;
    driver->state.debug.logElapsedTime = 0.0f;

    // 初始化HAL层
    if (HAL_Motor_Init(driver->halCfg) != HAL_OK)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "HAL motor initialization failed");
        return false;
    }

    return true;
}

void MotorDriver_SetMode(MotorDriver* driver, const MotorDriverMode mode)
{
    if (driver == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor driver is NULL");
        return;
    }

    // 模式切换处理
    if (driver->control.mode != mode)
    {
        switch (mode)
        {
            case MOTOR_DRIVER_MODE_CLOSED_LOOP:
                // 重置PID状态
                PID_Reset(&driver->control.pidCtrl);
                break;
            case MOTOR_DRIVER_MODE_OPEN_LOOP:
                // 保留当前占空比
                break;
            case MOTOR_DRIVER_MODE_STOP:
                HAL_Motor_EmergencyStop(driver->halCfg);
                break;
        }
        driver->control.mode         = mode;
        driver->state.lastUpdateTick = HAL_GetTick();
    }
}

void MotorDriver_SetDutyCycle(MotorDriver* driver, const float duty)
{
    if (driver == NULL || driver->state.mode != MOTOR_DRIVER_MODE_OPEN_LOOP)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Invalid call to SetDutyCycle");
        return;
    }

    // 限制占空比范围
    const float clamped_duty = (duty > 100.0f) ? 100.0f : (duty < -100.0f) ? -100.0f : duty;

    // 设置HAL层占空比
    HAL_Motor_SetDuty(driver->halCfg, clamped_duty);

    // 更新方向
    driver->state.direction = driver->halCfg->state.direction;

    // 更新状态
    driver->state.openLoopDuty = clamped_duty;
}

void MotorDriver_SetTargetRPM(MotorDriver* driver, const float rpm)
{
    if (driver == NULL)
    {
        return;
    }
    // 限制在规格范围内
    const float max_rpm       = driver->spec->maxRPM;
    driver->control.targetRPM = (rpm > max_rpm) ? max_rpm : (rpm < -max_rpm) ? -max_rpm : rpm;
}

void MotorDriver_Update(MotorDriver* driver)
{
    if (driver == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor driver is NULL");
        return;
    }

    // 更新模式
    driver->state.mode = driver->control.mode;

    // 获取时间增量（毫秒）
    const uint32_t current_tick = HAL_GetTick();
    const uint32_t last_tick    = driver->state.lastControlTick;

    // 计算时间增量（秒）
    float delta_time_s = 0.0f;
    if (current_tick > last_tick)
    {
        delta_time_s = (float)(current_tick - last_tick) * 0.001f;
    }

    // 更新时间太短则跳过
    if (delta_time_s < MIN_DELTA_TIME_S)
    {
        return;
    }
    // log delta_time_s
    // LOG_INFO(LOG_MODULE_MOTOR, "delta_time_s: %.6f, current_tick=%lu, last_tick=%lu", delta_time_s, (unsigned long)current_tick, (unsigned long)last_tick);

    // 更新时间戳
    driver->state.lastControlTick = current_tick;

    // 获取当前编码器脉冲值
    const uint32_t current_raw = HAL_Motor_ReadRawEncoder(driver->halCfg);

    // 计算脉冲增量（处理计数器溢出）
    const int32_t delta_pulses = calculate_delta_pulses(driver, current_raw);

    // 计算实际脉冲率（脉冲/秒）
    float pulses_per_sec = (float)delta_pulses / delta_time_s;

    // 更新位置信息（必须在速度计算后）
    driver->state.position.totalPulses += delta_pulses;
    update_revolutions(&driver->state, delta_pulses, driver->spec->encoderPPR, driver->spec->gearRatio);

    // 计算每转的实际计数值（考虑4倍频）
    const float counts_per_revolution        = 4.0f * (float)driver->spec->encoderPPR;
    const float counts_per_output_revolution = counts_per_revolution * (float)driver->spec->gearRatio;


    // 计算实际转速（RPM）计算公式: RPM = (脉冲率 × 60) / (PPR × 减速比)
    const float actual_rpm = (pulses_per_sec * 60.0f) / counts_per_output_revolution;

    // 应用速度滤波
    const float filtered_rpm = apply_speed_filter(driver, actual_rpm);

    // 更新速度状态
    driver->state.velocity.rpm   = filtered_rpm;
    driver->state.velocity.radps = rpm_to_radps(filtered_rpm);
    driver->state.velocity.mps   = radps_to_mps(driver->state.velocity.radps, driver->spec->wheelRadiusMM);


    // 闭环控制处理
    if (driver->state.mode == MOTOR_DRIVER_MODE_CLOSED_LOOP)
    {
        // 使用RPM直接归一化
        const float max_rpm           = driver->spec->maxRPM;
        const float normalized_target = driver->control.targetRPM / max_rpm;
        const float normalized_actual = filtered_rpm / max_rpm;
        // 限制归一化范围
        const float safe_target = fmaxf(fminf(normalized_target, 1.0f), -1.0f);
        const float safe_actual = fmaxf(fminf(normalized_actual, 1.0f), -1.0f);

        // 执行PID计算
        float output = PID_Calculate(&driver->control.pidCtrl, safe_target, safe_actual, delta_time_s, driver->id);

        // 添加前馈
        output += calculate_feedforward(driver->control.targetRPM, filtered_rpm);

        // 3. 应用PWM滤波
        const float filtered_output = apply_pwm_filter(driver, output);

        // 应用输出
        const float clamped_output = (filtered_output > 1.0f)
                                         ? 1.0f
                                         : (filtered_output < -1.0f)
                                         ? -1.0f
                                         : filtered_output;

        // // PID debug
        if (HAL_GetTick() - driver->state.debug.lastLogPrint >= 20)
        {
            // 50Hz采样率
            driver->state.debug.lastLogPrint = HAL_GetTick();
            driver->state.debug.logElapsedTime += 0.02f; // 20ms增量
            // LOG_INFO(LOG_MODULE_SYSTEM, "%d,%.2f,%.2f,%.2f\n",
            //          driver->id,
            //          safe_target * 100,
            //          safe_actual * 100,
            //          clamped_output * 100);
            //
            // LOG_INFO(LOG_MODULE_SYSTEM, "T=%.3f, ID=%d, Targ=%.2f, Act=%.2f, Out=%.2f, RPM=%.1f, Pulses=%ld",
            //          driver->state.debug.logElapsedTime,
            //          driver->id,
            //          safe_target * 100,
            //          safe_actual * 100,
            //          clamped_output * 100,
            //          filtered_rpm,
            //          driver->state.position.totalPulses);
        }

        HAL_Motor_SetDuty(driver->halCfg, clamped_output * 100.0f);

        // 更新时间戳（用于状态跟踪）
        driver->state.lastUpdateTick = current_tick;
    }
}


void MotorDriver_EmergencyStop(MotorDriver* driver)
{
    if (driver == NULL)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor driver is NULL");
        return;
    }

    HAL_Motor_EmergencyStop(driver->halCfg);
    driver->control.mode = MOTOR_DRIVER_MODE_STOP;
    PID_Reset(&driver->control.pidCtrl);
}

MotorDriverState MotorDriver_getState(const MotorDriver* driver)
{
    MotorDriverState state = {0};
    if (driver != NULL)
    {
        // 创建状态副本
        memcpy(&state, &driver->state, sizeof(MotorDriverState));
    }
    return state;
}

void MotorDriver_ConfigurePID(MotorDriver* driver, const PID_Params* params)
{
    if (!driver || !params)
    {
        LOG_ERROR(LOG_MODULE_MOTOR, "Motor driver or PID params is NULL");
        return;
    }

    // 拷贝PID参数到控制器
    memcpy(&driver->control.pidCtrl.params, params, sizeof(PID_Params));
    // 重置积分项和微分历史状态
    PID_Reset(&driver->control.pidCtrl);
}

//------------------------------------------------------------------------------
// 静态函数实现
//------------------------------------------------------------------------------

/**
 * @brief 将转速从转每分钟（RPM）转换为弧度每秒（rad/s）
 *
 * @param rpm 电机输出轴转速值，单位：转/分钟（Revolutions Per Minute）
 *           物理意义：电机输出轴（减速后）的每分钟转数
 * @return float 电机输出轴角速度值，单位：弧度/秒（rad/s）
 *
 * @note 转换公式：
 * rad/s = (RPM × 2π) / 60
 */
static inline float rpm_to_radps(const float rpm)
{
    return rpm * (2.0f * (float)M_PI) / 60.0f;
}


/**
 * @brief 将输出轴的角速度（rad/s）转换为输出轴的转速（RPM）
 *
 * @param radps 输出轴角速度，单位：弧度/秒（rad/s）
 * @return float 输出轴转速，单位：转/分钟（RPM）
 *
 * @note 公式：
 * RPM = (radps × 60) / (2π)
 * 示例：当 radps = 10.472 rad/s（≈ 100 RPM）时：
 * (10.472 × 60) / 6.283 ≈ 100 RPM
 */
static inline float radps_to_rpm(const float radps)
{
    return radps * 60.0f / (2.0f * (float)M_PI);
}


/**
 * @brief 将角速度转换为线速度
 *
 * @param radps 角速度值，单位：弧度/秒（rad/s）
 * @param wheel_radius_mm 轮子半径，单位：毫米（mm）
 * @return float 线速度值，单位：米/秒（m/s）
 *
 * @note 转换公式：
 * mps = radps × (wheelRadiusMM / 1000.0f)
 * 物理意义：轮缘线速度 = 角速度 × 轮半径（米制）
 *
 * 示例 当radps=10 rad/s，wheelRadiusMM=65 mm时：
 * 10 × (65 / 1000) = 0.65 m/s
 */
static inline float radps_to_mps(const float radps, const float wheel_radius_mm)
{
    return radps * (wheel_radius_mm / 1000.0f);
}


/**
 * @brief 将编码器脉冲数转换为旋转弧度值
 *
 * @param pulses 编码器脉冲数，单位：个（count）
 *              物理意义：四倍频后的累计脉冲数（包含方向信息）
 * @param encoder_ppr 编码器每转脉冲数，单位：脉冲/转（Pulses Per Revolution）
 *                  物理意义：编码器未倍频时每转输出的脉冲数
 * @param gear_ratio 减速比，单位：无因次量（如30:1减速比则传入30.0）
 * @return float 旋转角度值，单位：弧度（rad）
 *
 * @note 转换公式：
 * radians = (pulses × 2π) / (4 × PPR × gearRatio)
 * 四倍频说明：编码器每个脉冲周期产生4个计数（上升沿+下降沿×2通道）
 * 示例：encoderPPR=13, gearRatio=30, pulses=1560时：
 * (1560 × 6.2832) / (4 × 13 × 30) = 6.2832 rad（正好1转）
 */
static inline float pulses_to_rad(const int32_t pulses, const uint16_t encoder_ppr, const uint16_t gear_ratio)
{
    return ((float)pulses * (float)M_PI * 2.0f) / (4.0f * (float)encoder_ppr * (float)gear_ratio);
}


/**
 * @brief 更新累计转数值
 *
 * @param state 电机状态对象指针
 * @param delta_pulses 增量脉冲数，单位：个（count）
 * @param encoder_ppr 编码器每转脉冲数，单位：脉冲/转（Pulses Per Revolution）
 * @param gear_ratio 减速比，单位：无因次量
 *
 * @note 转换过程：
 * 1. 脉冲数转弧度：pulseToRad()
 * 2. 弧度转转数：radians / 2π
 * 公式：
 * revolutions += (deltaPulses × π × 2) / (4 × PPR × gearRatio) / (2π)
 * 简化为：
 * revolutions += deltaPulses / (4 × PPR × gearRatio)
 */
static void update_revolutions(MotorDriverState* state, const int32_t        delta_pulses,
                               const uint16_t    encoder_ppr, const uint16_t gear_ratio)
{
    state->position.revolutions += (float)delta_pulses / (4.0f * (float)encoder_ppr * (float)gear_ratio);
}

static float apply_speed_filter(MotorDriver* driver, float raw_speed)
{
    // 未启用滤波
    if (driver->filter.windowSize == 0 || driver->filter.filterBuffer == NULL)
    {
        return raw_speed;
    }

    // 添加新采样值
    driver->filter.filterBuffer[driver->filter.bufferIndex] = raw_speed;

    // 加权移动平均
    float         sum          = 0.0f;
    float         weight_sum   = 0.0f;
    const uint8_t center_index = driver->filter.windowSize / 2;
    for (uint8_t i = 0; i < driver->filter.windowSize; i++)
    {
        const float weight = 1.0f - fabsf((float)(i - center_index)) / (float)(center_index + 1);
        sum += driver->filter.filterBuffer[i] * weight;
        weight_sum += weight;
    }
    const float ma_filtered = sum / weight_sum;

    // 更新索引
    driver->filter.bufferIndex = (driver->filter.bufferIndex + 1) % driver->filter.windowSize;

    // 二级低通滤波
    const float alpha        = 0.5f; // 滤波系数
    driver->filter.lpf_state = alpha * ma_filtered + (1 - alpha) * driver->filter.lpf_state;
    return driver->filter.lpf_state;
}

//  计算脉冲增量
int32_t calculate_delta_pulses(MotorDriver* driver, const uint32_t current_raw)
{
    const uint32_t maxValue = HAL_Motor_GetEncoderMaxValue(driver->halCfg);
    const int32_t  maxVal   = (int32_t)maxValue;
    const int32_t  halfMax  = maxVal / 2;
    const uint32_t last_raw = driver->state.position.lastRawPulses;

    // 首次调用初始化
    if (last_raw == 0 && current_raw != 0)
    {
        driver->state.position.lastRawPulses = current_raw;
        return 0;
    }

    int32_t delta = (int32_t)current_raw - (int32_t)last_raw;
    // 处理正向回绕（0 → maxValue）
    if (delta < -halfMax)
    {
        delta += maxVal + 1;
    }
    // 处理反向回绕（maxValue → 0）
    else if (delta > halfMax)
    {
        delta -= maxVal + 1;
    }

    // 更新上次值
    driver->state.position.lastRawPulses = current_raw;

    return delta;
}

// 应用PWM滤波
float apply_pwm_filter(MotorDriver* driver, float raw_pwm)
{
    driver->filter.pwm_filter_state = driver->filter.pwm_alpha * raw_pwm +
                                      (1 - driver->filter.pwm_alpha) * driver->filter.pwm_filter_state;
    return driver->filter.pwm_filter_state;
}

float calculate_feedforward(float target_rpm, float filtered_rpm)
{
    // 1. 确定旋转方向
    const float direction  = (target_rpm >= 0.0f) ? 1.0f : -1.0f;
    const float abs_target = fabsf(target_rpm);
    const float abs_rpm    = fabsf(filtered_rpm);

    // 2. 基础前馈
    const float FEEDFORWARD_GAIN = 0.0028f;
    float       base_ff          = abs_target * FEEDFORWARD_GAIN * direction;

    // === 关键改进：平滑启动处理 ===

    // 2.1 低速前馈增益调整
    if (abs_target < 20.0f)
    {
        float gain_factor = 1.0f;
        // 从0.7到1.0的平滑过渡
        gain_factor = 0.7f + 0.3f * (abs_target / 20.0f);
        base_ff *= gain_factor;
    }

    // 2.2 启动曲线（0-15 RPM）
    if (abs_target <= 15.0f && abs_rpm < 5.0f)
    {
        float startup_curve = 1.0f;
        // 从0.3到1.0的平滑过渡
        const float startup_factor = fmaxf(0.3f, abs_target / 15.0f);
        startup_curve              = 0.7f * startup_factor + 0.3f;
        base_ff *= startup_curve;
    }

    // 3. 自适应摩擦补偿
    float friction_comp = 0.0f;

    // 3.1 静摩擦补偿（随目标RPM和速度平滑变化）
    if (abs_rpm < 5.0f && abs_target > 0.1f)
    {
        // 静摩擦随目标RPM线性变化 (0.01-0.03)
        const float target_factor   = fminf(abs_target / 50.0f, 1.0f);
        const float static_friction = 0.01f + 0.02f * target_factor;

        // 随速度平滑过渡到动摩擦
        const float speed_factor = 1.0f - fminf(abs_rpm / 5.0f, 1.0f);
        friction_comp            = static_friction * speed_factor * direction;
    }
    // 3.2 动摩擦补偿（随速度增加）
    else if (abs_target > 0.1f)
    {
        const float dynamic_friction = 0.005f + 0.005f * (abs_rpm / 50.0f);
        friction_comp                = dynamic_friction * direction;
    }


    // 5. 组合所有前馈项
    float feedforward = base_ff + friction_comp;

    // 6. 启动阶段额外限幅（关键！）
    if (abs_rpm < 5.0f && abs_target > 10.0f)
    {
        // 启动阶段限制最大前馈
        const float max_startup = 0.08f + 0.02f * (abs_target / 50.0f);
        feedforward             = CLAMP(feedforward, -max_startup, max_startup);
    }

    // 7. 全局限幅
    return CLAMP(feedforward, -1.0f, 1.0f);
}
