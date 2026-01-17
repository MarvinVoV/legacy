#pragma once
//
// Created by marvin on 2025/2/16.
//


// 日志级别定义
typedef enum
{
    MQTT_LOG_ERROR,
    MQTT_LOG_WARN,
    MQTT_LOG_INFO,
    MQTT_LOG_DEBUG
} mqtt_log_level_t;

// 模块初始化
void mqtt_logger_init();

// 核心日志函数
void mqtt_log_write(mqtt_log_level_t level, const char* tag, const char* format, ...);

// 快捷宏
#define LOG_E(tag, format, ...) mqtt_log_write(MQTT_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define LOG_W(tag, format, ...) mqtt_log_write(MQTT_LOG_WARN,  tag, format, ##__VA_ARGS__)
#define LOG_I(tag, format, ...) mqtt_log_write(MQTT_LOG_INFO,  tag, format, ##__VA_ARGS__)
#define LOG_D(tag, format, ...) mqtt_log_write(MQTT_LOG_DEBUG, tag, format, ##__VA_ARGS__)
