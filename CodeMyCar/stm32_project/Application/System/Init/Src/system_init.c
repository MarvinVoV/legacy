/*
 * system_init.c
 *
 *  Created on: Apr 12, 2025
 *      Author: marvin
 */
#include "system_init.h"
#include "error_code.h"


/**
 * @brief 底盘默认物理参数配置
 *
 * @note 所有长度单位统一为毫米(mm)，角度单位为度(deg)
 */
static const ChassisConfig DEFAULT_CHASSIS_CFG = {
    .wheelbaseMM = 144.0f,        // 轴距（前后轮中心距），单位：毫米(mm)
    .trackWidthMM = 167.0f,       // 轮距（左右轮中心距），单位：毫米(mm)
    .wheelRadiusMM = 32.5f,       // 驱动轮半径，单位：毫米(mm)
    .maxLinearVelocityMPS = 1.2f, // 最大允许线速度，单位：米/秒(m/s) 正值为前进，负值为后退
    .maxAngularVelRad = 8.2f,     // 最大允许角速度，单位：弧度/秒(rad/s)
    .maxSteerAngleDeg = 45.0f,     // 前轮最大转向角度，单位：度(degree) 正值右转，负值左转
    .steerCenterAngleDeg = 90.0f, // 归中角度
};

// // 舵机参数
// #define SERVO_ANGLE_MIN  0.0f  // 最小角度 (度)
// #define SERVO_ANGLE_MAX  180.0f // 最大角度 (度)


// 系统上下文
static SystemContext sysCtx;

// 模块实例
static MotorService        motorService;
static SteerInstance       steerInstance;
static MotionContext       motionContext;
static CmdProcessorContext commandContext;

// 驱动实例
static MotorDriver motorLeftDriver;
static MotorDriver motorRightDriver;
static ServoDriver servoDriver;

// 全局静态变量
static HAL_MotorConfig leftHalCfg;
static HAL_MotorConfig rightHalCfg;
static MotorSpec       leftMotorSpec;
static MotorSpec       rightMotorSpec;
static PID_Controller  leftPidControllerInstance;
static PID_Controller  rightPidControllerInstance;

// 初始化状态
static bool isInitialized = false;


/**
 * @brief 初始化电机服务
 *
 * @note 依赖：必须在舵机初始化前调用
 * @note 依赖：必须在定时器初始化完成后调用
 *
 * @return 错误码
 */
static int init_motor_service(void)
{
    // 左轮电机配置
    rightHalCfg = (HAL_MotorConfig){
        .pwm = {.tim = &htim3, .ch = TIM_CHANNEL_2},
        .encoder = {&htim5},
        .gpio = {
            .in1Port = GPIOF, .in1Pin = GPIO_PIN_6,
            .in2Port = GPIOF, .in2Pin = GPIO_PIN_10
        }
    };

    // 右轮电机配置
    leftHalCfg = (HAL_MotorConfig){
        .pwm = {.tim = &htim3, .ch = TIM_CHANNEL_1},
        .encoder = {&htim4},
        .gpio = {
            .in1Port = GPIOF, .in1Pin = GPIO_PIN_7,
            .in2Port = GPIOF, .in2Pin = GPIO_PIN_8
        }
    };

    // 电机规格参数
    leftMotorSpec = (MotorSpec){
        .encoderPPR = 13,
        .gearRatio = 30,
        .maxRPM = 300,
        .wheelRadiusMM = DEFAULT_CHASSIS_CFG.wheelRadiusMM
    };
    rightMotorSpec = (MotorSpec){
        .encoderPPR = 13,
        .gearRatio = 30,
        .maxRPM = 300,
        .wheelRadiusMM = DEFAULT_CHASSIS_CFG.wheelRadiusMM
    };

    // 左轮PID参数
    leftPidControllerInstance = (PID_Controller){
        .params = {
            .kp = 0.68f,                  // 提高比例增益（增强启动能力）
            .ki = 0.9f,                   // 提高积分增益（加速误差消除）
            .kd = 0.09f,                  // 降低微分增益（减少噪声影响）
            .output_max = 1.0f,           // 输出限幅（100%占空比）
            .integral_threshold = 0.005f, // 误差阈值
            .dead_zone = 0.002f
        },
        .state = {
            .first_run = true,
            .prev_error = 0.0f,
            .prev_measurement = 0.0f,
            .prev_output = 0.0f,
            .direction_stable = true, // 新增状态：方向稳定性
            .stable_threshold = 0.1f  // 稳定阈值
        }
    };

    // 右轮PID参数
    rightPidControllerInstance = (PID_Controller){
        .params = {
            .kp = 0.68f,                  // 提高比例增益（增强启动能力）
            .ki = 0.9f,                   // 提高积分增益（加速误差消除）
            .kd = 0.09f,                  // 降低微分增益（减少噪声影响）
            .output_max = 1.0f,           // 输出限幅（100%占空比）
            .integral_threshold = 0.005f, // 误差阈值
            .dead_zone = 0.002f
        },
        .state = {
            .first_run = true,
            .prev_error = 0.0f,
            .prev_measurement = 0.0f,
            .prev_output = 0.0f,
            .direction_stable = true, // 新增状态：方向稳定性
            .stable_threshold = 0.1f  // 稳定阈值
        }
    };

    motorLeftDriver = (MotorDriver){
        .id = 0,
        .halCfg = &leftHalCfg,
        .spec = &leftMotorSpec,
        .control = {
            .mode = MOTOR_DRIVER_MODE_STOP,
            .pidCtrl = leftPidControllerInstance,
            .targetRPM = 0.0f,
        }
    };

    motorRightDriver = (MotorDriver){
        .id = 1,
        .halCfg = &rightHalCfg,
        .spec = &rightMotorSpec,
        .control = {
            .mode = MOTOR_DRIVER_MODE_STOP,
            .pidCtrl = rightPidControllerInstance,
            .targetRPM = 0.0f,
        }
    };

    motorService = (MotorService){
        .instances = {
            [MOTOR_LEFT_WHEEL] = {
                .driver = &motorLeftDriver
            },
            [MOTOR_RIGHT_WHEEL] = {
                .driver = &motorRightDriver
            }
        }
    };
    return MotorService_init(&motorService, &motorLeftDriver, &motorRightDriver);
}

/**
 * @brief 初始化转向服务
 *
 * @note 依赖：必须在舵机PWM定时器初始化完成后调用
 * @note 依赖：必须在电机服务初始化后调用（无技术依赖，但建议顺序）
 *
 * @return 错误码
 */
static int init_steer_servo_service(void)
{
    static HAL_ServoConfig servoHalConfig = (HAL_ServoConfig){
        .pwmTim = &htim2,
        .channel = TIM_CHANNEL_1,
        .maxPulse = SERVO_MAX_PULSE,
        .minPulse = SERVO_MIN_PULSE
    };

    static SteerConfig steerConfig = (SteerConfig){
        .minAngleDeg = SERVO_ANGLE_MIN,
        .maxAngleDeg = SERVO_ANGLE_MAX,
        .deadZoneDeg = 2,
        .updateIntervalMs = 20,
        .maxSpeedDegPerSec = 90.0f,      // 最大速度：90°/秒（即 0.025°/ms）
        .accelerationDegPerSec2 = 180.0f // 加速度：180°/秒²（即 0.18°/ms²）
    };

    servoDriver = (ServoDriver){
        .hw = &servoHalConfig,
        .spec = {
            .minAngle = SERVO_ANGLE_MIN,
            .maxAngle = SERVO_ANGLE_MAX,
            .minPulseUs = SERVO_MIN_PULSE,
            .maxPulseUs = SERVO_MAX_PULSE,
            .resolutionDeg = 0.1f
        }
    };

    steerInstance = (SteerInstance){
        .driver = &servoDriver
    };

    return SteerService_init(&steerInstance, &steerConfig, &servoDriver);
}


/**
 * @brief 初始化运动控制上下文
 *
 * @note 依赖：必须在电机服务和转向服务初始化完成后调用
 *
 * @return 错误码
 */
static int init_motion_context(void)
{
    motionContext = (MotionContext){
        .chassisConfig = DEFAULT_CHASSIS_CFG,
        .motorService = &motorService,
        .steerServo = &steerInstance
    };
    return MotionContext_Init(&motionContext, &motorService, &steerInstance, &DEFAULT_CHASSIS_CFG);
}

/**
 * @brief 初始化命令处理器
 *
 * @note 依赖：必须在运动控制上下文初始化完成后调用
 *
 * @return 错误码
 */
static int init_cmd_processor(void)
{
    // 初始化命令处理器
    commandContext = (CmdProcessorContext){
        .motionContext = &motionContext
    };
    return CmdProcessor_Init(&commandContext, &motionContext);
}

int System_Initialize()
{
    if (isInitialized)
    {
        LOG_WARN(LOG_MODULE_SYSTEM, "System already initialized");
        return ERR_SUCCESS;
    }

    int ret = ERR_SUCCESS;

    // 清零所有状态
    memset(&sysCtx, 0, sizeof(SystemContext));
    memset(&motorService, 0, sizeof(MotorService));
    memset(&steerInstance, 0, sizeof(SteerInstance));
    memset(&motionContext, 0, sizeof(MotionContext));
    memset(&commandContext, 0, sizeof(CmdProcessorContext));

    // 初始化顺序：
    // 1. 电机服务（依赖定时器和GPIO）
    if ((ret = init_motor_service()) != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Motor service init failed: 0x%04X", ret);
        return ret;
    }


    // 2. 转向服务（依赖舵机PWM定时器）
    if ((ret = init_steer_servo_service()) != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Steer service init failed: 0x%04X", ret);
        return ret;
    }

    // 3. 运动控制（依赖电机和转向服务）
    if ((ret = init_motion_context()) != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Motion context init failed: 0x%04X", ret);
        return ret;
    }

    // 4. 命令处理器（依赖运动控制）
    if ((ret = init_cmd_processor()) != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Cmd processor init failed: 0x%04X", ret);
        return ret;
    }

    // 设置系统上下文
    sysCtx.motionContext = &motionContext;
    sysCtx.motorService  = &motorService;
    sysCtx.cmdContext    = &commandContext;
    sysCtx.steerInstance = &steerInstance;

    isInitialized = true;
    LOG_INFO(LOG_MODULE_SYSTEM, "System initialized successfully");
    return ERR_SUCCESS;
}

SystemContext* System_GetContext(void)
{
    return isInitialized ? &sysCtx : NULL;
}
