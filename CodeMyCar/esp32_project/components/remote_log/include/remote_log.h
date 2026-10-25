#pragma once

/**
 * @file remote_log.h
 * @brief 远程 MQTT 日志（静态 buffer pool，零 malloc 热路径）
 */

#include "esp_err.h"

typedef enum {
    REMOTE_LOG_ERROR = 0,
    REMOTE_LOG_WARN  = 1,
    REMOTE_LOG_INFO  = 2,
    REMOTE_LOG_DEBUG = 3,
} remote_log_level_t;

esp_err_t remote_log_init(void);
esp_err_t remote_log_start(void);

/** 写日志（格式化后入队，由 log_task 异步发布到 MQTT） */
void remote_log_write(remote_log_level_t level, const char* tag,
                      const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* 快捷宏 */
#define RLOG_E(tag, fmt, ...) remote_log_write(REMOTE_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define RLOG_W(tag, fmt, ...) remote_log_write(REMOTE_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define RLOG_I(tag, fmt, ...) remote_log_write(REMOTE_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define RLOG_D(tag, fmt, ...) remote_log_write(REMOTE_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
