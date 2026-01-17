#include "log_manager.h"

#include <stdio.h>
#include <stdarg.h>

#include "freertos_os2.h"
#include "log_task.h"
#include "queue_manager.h"


/// @brief 入队超时配置 2ms
#define LOG_QUEUE_WAIT_TICKS (2)

/// @brief 日志队列句柄（存储log_entry_t指针）
static osMessageQueueId_t log_queue = NULL;

/// @brief 注册的输出通道数组
static log_output_channel_t* registered_channels[LOG_MAX_CHANNELS] = {NULL};

//------------------------------------------------------------------------------
// 内部函数声明
//------------------------------------------------------------------------------
/**
 * @brief 分发日志条目到注册的输出通道
 *
 * @param entry 待分发的日志条目
 *
 * @note 根据通道配置决定直接输出或入队
 */
static void dispatch_log_entry(log_entry_t* entry);

//------------------------------------------------------------------------------
// 公共API实现
//------------------------------------------------------------------------------

/**
 * @brief 注册日志输出通道
 *
 * @param channel 通道配置指针
 *
 * @retval true  注册成功
 * @retval false 注册失败（通道数已达上限）
 *
 * @note 通道配置必须在整个生命周期保持有效
 */
bool LogManager_AddOutputChannel(log_output_channel_t* channel)
{
    if (channel == NULL)
    {
        return false;
    }

    for (int i = 0; i < LOG_MAX_CHANNELS; i++)
    {
        if (registered_channels[i] == NULL)
        {
            registered_channels[i] = channel;
            return true;
        }
    }

    return false; // 所有通道槽位已满
}

/**
 * @brief 初始化日志管理器
 *
 * 创建日志队列和关联资源
 *
 * @note 必须在RTOS启动后调用
 * @note 调用前应注册所有输出通道
 */
void LogManager_Init(void)
{
    // 创建消息队列（存储log_entry_t指针）
    log_queue = osMessageQueueNew(LOG_QUEUE_SIZE, sizeof(log_entry_t*), NULL);
    if (log_queue == NULL)
    {
        /* 处理错误 */
        // TODO
    }

    // 注册消息队列到队列管理器
    QueueManager_registerHandler(QUEUE_TYPE_LOG, log_queue);

    // 初始化日志任务
    LogTask_Init();
}

bool LogManager_Config(log_manager_config_t* config)
{
    if (config == NULL || config->channel_count > LOG_MAX_CHANNELS)
    {
        return false;
    }

    // 初始化日志系统
    LogManager_Init();

    // 注册所有通道
    for (int i = 0; i < config->channel_count; i++)
    {
        LogManager_AddOutputChannel(&config->channels[i]);
    }

    // 在支持热插拔的系统中可以保留此接口
    return true;
}


/**
 * @brief 记录日志条目
 *
 * @param level   日志级别 (log_level_t)
 * @param module  来源模块 (log_module_t)
 * @param format  格式字符串（类printf）
 * @param ...     可变参数
 *
 * @note 内存分配失败时静默丢弃日志
 */
void log_record(const log_level_t level, const log_module_t module, const char* format, ...)
{
    /* 参数有效性检查 */
    if (format == NULL ||
        (level & LOG_ENABLED_LEVELS) == 0 ||
        (module & LOG_ENABLED_MODULES) == 0)
    {
        return;
    }


    va_list      args;
    int          formatted_length = 0;
    log_entry_t* entry            = NULL;
    size_t       required_size    = 0;

    // 步骤1: 计算格式化后消息长度
    va_start(args, format);
    formatted_length = vsnprintf(NULL, 0, format, args);
    va_end(args);

    // 检查长度有效性
    if (formatted_length < 0)
    {
        return; // 格式错误
    }

    // 添加终止符空间 (+1) 并应用长度限制
    formatted_length = (formatted_length < MAX_MESSAGE_LENGTH) ? formatted_length : MAX_MESSAGE_LENGTH - 1;

    // 步骤2: 分配内存（结构体 + 消息缓冲区）
    required_size = sizeof(log_entry_t) + formatted_length + 1; // +1 for null terminator
    entry         = (log_entry_t*)pvPortMalloc(required_size);

    if (entry == NULL)
    {
        // 内存分配失败 - 无法记录日志
        // TODO  vApplicationMallocFailedHook()
        return;
    }
    // 初始化结构体
    memset(entry, 0, sizeof(log_entry_t));
    entry->timestamp = osKernelGetTickCount();
    entry->module    = module;
    entry->level     = level;


    memset(entry, 0, sizeof(log_entry_t));
    /* 填充基础信息 */
    entry->timestamp = osKernelGetTickCount();
    entry->module    = module;
    entry->level     = level;

    // 步骤3: 实际格式化消息
    va_start(args, format);
    const int actual_length = vsnprintf(entry->message, formatted_length + 1, format, args);
    va_end(args);

    // 确保终止符（即使格式化出错）
    if (actual_length < 0 || actual_length > formatted_length)
    {
        strncpy(entry->message, "[LOG FORMAT ERROR]", formatted_length);
        entry->message[formatted_length] = '\0';
    }
    else
    {
        entry->message[actual_length] = '\0'; // 确保终止符
    }

    // 分发日志条目
    dispatch_log_entry(entry);
}

/**
 * @brief 分发日志条目到注册的输出通道
 *
 * @param entry 待分发的日志条目
 */
static void dispatch_log_entry(log_entry_t* entry)
{
    bool entry_queued           = false;
    bool direct_output_occurred = false;

    for (int i = 0; i < LOG_MAX_CHANNELS; i++)
    {
        if (registered_channels[i] == NULL)
        {
            continue;
        }
        const log_output_channel_t* channel = registered_channels[i];

        // 检查通道是否对该日志感兴趣
        if ((channel->level_mask & entry->level) == 0 || (channel->module_mask & entry->module) == 0)
        {
            continue; // 通道不关心此日志
        }

        if (channel->direct_output)
        {
            // 直接输出通道：立即输出
            if (channel->huart != NULL)
            {
                DEBUG_CONSOLE_OUTPUT(channel->huart, "%s\n", entry->message);
                direct_output_occurred = true;
            }
        }
        else
        {
            // 异步输出通道：入队处理
            const osStatus_t status = osMessageQueuePut(log_queue, &entry, osPriorityNone,
                                                        LOG_QUEUE_WAIT_TICKS);
            if (status == osOK)
            {
                entry_queued = true;
            }
            else if (status == osErrorResource)
            {
                // 队列满 - 实现可选的丢弃策略
                #ifdef LOG_DROP_ON_QUEUE_FULL
                // 可添加丢弃计数统计
                #endif
            }
            else
            {
                // 其他错误（如队列无效）
                DEBUG_CONSOLE_OUTPUT(NULL, "[LOG] Queue error: %d\n", status);
            }
        }
    }
    // 资源清理策略
    if (entry_queued)
    {
        // 条目已成功入队 - 将由日志任务释放
        return;
    }
    if (direct_output_occurred)
    {
        // 仅直接输出发生 - 立即释放
        vPortFree(entry);
    }
    else
    {
        // 无通道处理此日志 - 立即释放
        vPortFree(entry);
    }
}
