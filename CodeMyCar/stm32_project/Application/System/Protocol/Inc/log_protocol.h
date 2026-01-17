/*
 * log_protocol.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef SYSTEM_PROTOCOL_INC_LOG_PROTOCOL_H_
#define SYSTEM_PROTOCOL_INC_LOG_PROTOCOL_H_

#include <stdint.h>
#include <string.h>

#include "pkt_protocol.h"

/**
 * @brief 日志内容的最大字节容量（非字符串长度）
 *
 * 定义日志消息部分的最大字节数，包含以下关键约束：
 * 1. 实际限制: 当前配置 = `(PROTOCOL_MAX_DATA_LEN - 8)` 字节
 * 2. 内存特性: 包含原始二进制数据，不保证NUL终止
 *
 * @warning 使用strcpy()存在溢出风险，必须使用memcpy()操作
 * @note 有效字符数通常比该值少1（如需NUL终止）
 */
#define MAX_MESSAGE_LENGTH (PROTOCOL_MAX_DATA_LEN - 8)

/**
 * @brief 日志级别枚举（按位掩码设计）
 *
 * 每个日志级别使用独立的比特位，支持通过位运算组合多个级别。
 * 例如：LOG_LEVEL_DEBUG | LOG_LEVEL_ERROR 表示同时包含调试和错误信息。
 *
 * @note 使用位掩码过滤时，可通过 (level & mask) != 0 判断是否包含某级别
 */
typedef enum
{
    LOG_LEVEL_DEBUG    = 0x01, ///< 调试信息 - 详细的开发过程跟踪
    LOG_LEVEL_INFO     = 0x02, ///< 常规信息 - 系统正常运行状态
    LOG_LEVEL_WARNING  = 0x04, ///< 警告信息 - 潜在问题但系统仍可运行
    LOG_LEVEL_ERROR    = 0x08, ///< 错误信息 - 功能异常但可恢复
    LOG_LEVEL_CRITICAL = 0x10, ///< 严重错误 - 系统关键功能失效
    LOG_LEVEL_ALL      = 0xFF  ///< 所有级别 - 用于日志过滤设置
} log_level_t;

/**
 * @brief 日志模块标识枚举（按位掩码设计）
 *
 * 每个模块使用独立的比特位，支持通过位运算选择多个模块组合。
 *
 * @warning 枚举值必须为2的幂（单比特位）以确保正确组合
 */
typedef enum
{
    LOG_MODULE_SYSTEM  = 0x0001, ///< 系统核心模块（初始化、任务调度等）
    LOG_MODULE_MOTOR   = 0x0002, ///< 电机控制模块（驱动、PID算法等）
    LOG_MODULE_SENSOR  = 0x0004, ///< 传感器模块（数据采集、滤波等）
    LOG_MODULE_NETWORK = 0x0008, ///< 网络通信模块（TCP/IP、协议栈等）
    LOG_MODULE_DRIVER  = 0x0010, ///< 设备驱动模块（外设初始化、控制等）
    LOG_MODULE_SERVO   = 0x0020, ///< 舵机控制模块（PWM生成、角度控制等）
    LOG_MODULE_MOTION  = 0x0040, ///< 运动控制模块（轨迹规划、插补等）
    LOG_MODULE_CMD     = 0x0080, ///< 指令处理模块（命令解析、执行等）
    LOG_MODULE_ALL     = 0xFFFF  ///< 所有模块 - 用于日志过滤设置
} log_module_t;

/**
 * @brief 日志条目结构体（内存紧凑布局）
 *
 * @note 使用#pragma pack(1)确保无填充字节，节省存储空间
 */
#pragma pack(push, 1)
typedef struct
{
    /// 系统启动后的毫秒时间戳（从bootloader启动计时）
    uint32_t timestamp;

    /// 日志来源模块标识符，按位组合支持多模块
    uint16_t module;

    /// 日志严重级别，支持按位过滤
    uint8_t level;

    /**
     * 可变长度日志内容（柔性数组）
     *
     * - 存储时需要预分配内存: sizeof(log_entry_t) + MAX_MESSAGE_LENGTH + 1
     * - 最大长度由内存分配决定（通常MAX_MESSAGE_LENGTH=128）
     * - 使用时需确保目标缓冲区足够容纳本结构+内容
     */
    char message[];
} log_entry_t;
#pragma pack(pop)

#define LOG_TIMESTAMP_BASE HAL_GetTick()  // 时间戳基准

#endif /* SYSTEM_PROTOCOL_INC_LOG_PROTOCOL_H_ */
