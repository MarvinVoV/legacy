/*
 * log_manager.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef MODULES_LOG_INC_LOG_MANAGER_H_
#define MODULES_LOG_INC_LOG_MANAGER_H_

#include <stdbool.h>

#include "cmsis_os2.h"
#include "log_protocol.h"
#include "stm32h7xx_hal.h"
//------------------------------------------------------------------------------
// 日志过滤配置（编译时可覆盖）
//------------------------------------------------------------------------------

/**
 * @brief 启用的日志级别掩码
 *
 * 通过位或(|)组合选择启用的日志级别：
 * - 默认：INFO/WARNING/ERROR/CRITICAL
 * - DEBUG级别默认不启用（性能考虑）
 *
 * @note 编译前定义可覆盖默认值
 * @see log_level_t
 */
#ifndef LOG_ENABLED_LEVELS
#define LOG_ENABLED_LEVELS \
  ((LOG_LEVEL_INFO) | (LOG_LEVEL_WARNING) | (LOG_LEVEL_ERROR) | (LOG_LEVEL_CRITICAL))
#endif

/**
 * @brief 启用的日志模块掩码
 *
 * 通过位或(|)组合选择启用的模块：
 * - 默认：所有模块(LOG_MODULE_ALL)
 * - 可按需启用特定模块组合
 *
 * @note 编译前定义可覆盖默认值
 * @see log_module_t
 */
#ifndef LOG_ENABLED_MODULES
#define LOG_ENABLED_MODULES (LOG_MODULE_ALL)
#endif

//------------------------------------------------------------------------------
// 日志系统资源限制配置
//------------------------------------------------------------------------------

/**
 * @brief 最大日志输出通道数
 *
 * 支持同时配置的输出目标数量（如UART/网络）：
 */
#define LOG_MAX_CHANNELS (2)

/**
 * @brief 日志队列最大条目数
 *
 * 异步日志的缓冲队列容量：
 * - 队列满时新日志将被丢弃（非阻塞策略）
 * - 默认值16基于典型嵌入式内存配置
 *
 * @note 内存占用 ≈ (sizeof(log_entry_t) + 32) × 本值
 */
#define LOG_QUEUE_SIZE (16)

/**
 * @brief 日志内存池区块数
 *
 * 静态预分配的内存块数量：
 * - 每个区块存储一个log_entry_t
 * - 避免动态内存分配，提高实时性
 *
 * @warning 推荐：LOG_POOL_BLOCKS ≥ LOG_QUEUE_SIZE × 1.5
 */
#define LOG_POOL_BLOCKS  (16)

//------------------------------------------------------------------------------
// 日志系统核心API
//------------------------------------------------------------------------------


extern osMemoryPoolId_t log_pool; ///< 日志内存池句柄

/**
 * @brief 日志记录实现函数
 *
 * 格式化日志内容并提交到日志系统
 *
 * @param[in] level   日志级别 (log_level_t)
 * @param[in] module  来源模块 (log_module_t)
 * @param[in] format  格式字符串（类printf）
 * @param[in] ...     可变参数
 *
 * @note 实际输出取决于注册的输出通道
 */
void log_record(log_level_t level, log_module_t module, const char* format, ...);


/**
 * @brief 条件日志记录宏
 *
 * 在启用对应级别和模块时才记录日志
 */
#define LOG(level, module, ...)                                       \
  do {                                                                \
    if ((level & LOG_ENABLED_LEVELS) && (module & LOG_ENABLED_MODULES)) { \
      log_record(level, module, __VA_ARGS__);                         \
    }                                                                 \
  } while (0)

/**
 * @brief 直接调试输出（绕过日志系统）
 *
 * @param huart 目标UART句柄（NULL则跳过）
 * @param ...   格式化字符串及参数
 *
 * @warning 阻塞式操作！仅用于调试阶段
 * @warning 输出内容不存储、不包含时间戳
 */
#define DEBUG_CONSOLE_OUTPUT(huart, ...) \
do { \
    if ((huart) != NULL) { \
      char _dbg_buf[128]; \
      int _len = snprintf(_dbg_buf, sizeof(_dbg_buf), __VA_ARGS__); \
      if (_len > 0) { \
        HAL_UART_Transmit((huart), (uint8_t*)_dbg_buf, \
            (_len < sizeof(_dbg_buf)) ? _len : sizeof(_dbg_buf)-1, 10); \
      } \
    } \
} while(0)

//------------------------------------------------------------------------------
// 预定义快捷日志接口
//------------------------------------------------------------------------------
#define LOG_DEBUG(module, ...) LOG(LOG_LEVEL_DEBUG, module, __VA_ARGS__)
#define LOG_INFO(module, ...) LOG(LOG_LEVEL_INFO, module, __VA_ARGS__)
#define LOG_WARN(module, ...) LOG(LOG_LEVEL_WARNING, module, __VA_ARGS__)
#define LOG_ERROR(module, ...) LOG(LOG_LEVEL_ERROR, module, __VA_ARGS__)
#define LOG_CRITICAL(module, ...) LOG(LOG_LEVEL_CRITICAL, module, __VA_ARGS__)


//------------------------------------------------------------------------------
// 日志输出通道管理
//------------------------------------------------------------------------------


/**
 * @brief 日志输出通道配置结构
 */
typedef struct
{
    UART_HandleTypeDef* huart;         ///< 物理UART接口（设为NULL则禁用）
    log_level_t         level_mask;    ///< 通道接收的级别掩码（位或组合）
    log_module_t        module_mask;   ///< 通道接收的模块掩码（位或组合）
    bool                direct_output; ///< 是否直出（true：忽略队列）
} log_output_channel_t;

/**
 * @brief 日志系统配置结构体
 */
typedef struct
{
    log_output_channel_t channels[LOG_MAX_CHANNELS]; ///< 通道数组
    uint8_t              channel_count;              ///< 实际使用的通道数
} log_manager_config_t;

/**
 * @brief 初始化日志管理器
 *
 * 创建日志任务、队列和内存池
 *
 * @note 必须在RTOS启动后调用
 * @note 调用前应注册所有输出通道
 */
void LogManager_Init(void);
/**
 * @brief 注册日志输出通道
 *
 * @param[in] channel 通道配置指针
 *
 * @retval true  注册成功
 * @retval false 注册失败（通道数超限或配置无效）
 *
 * @note 必须在LogManager_Init()之后调用
 */
bool LogManager_AddOutputChannel(log_output_channel_t* channel);

/**
 * @brief 配置并初始化日志系统
 *
 * @param config 日志系统配置
 *
 * @retval true  初始化成功
 * @retval false 初始化失败
 *
 * @note 此函数执行以下操作：
 *        1. 初始化日志管理器（队列、内存池等）
 *        2. 注册所有配置的日志通道
 *        3. 启动日志任务
 *
 * @warning 应在系统初始化完成后调用
 */
bool LogManager_Config(log_manager_config_t* config);


#endif /* MODULES_LOG_INC_LOG_MANAGER_H_ */
