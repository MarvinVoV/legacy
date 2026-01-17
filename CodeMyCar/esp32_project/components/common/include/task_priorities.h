/**
 * @brief 任务优先级统一管理头文件
 *
 * 定义系统中所有任务的优先级枚举，实现优先级集中管理
 */

#pragma once

/**
 * @brief 系统任务优先级枚举定义
 */
typedef enum
{
    TASK_PRIORITY_IDLE = 0,          // 系统空闲任务（保留）
    TASK_PRIORITY_EVENT_HANDLER = 9, // 事件处理任务（最高优先级）
    // UART
    TASK_PRIORITY_UART_EVENT = 8,           // UART事件处理任务
    TASK_PRIORITY_UART_HANDLE_MQTT_MSG = 7, // UART缓冲区处理任务
    TASK_PRIORITY_UART_BUFFER_PROCESS = 6,  // UART缓冲区处理任务
    // MQTT
    TASK_PRIORITY_MQTT_HANDLE_UART_MSG = 5, // MQTT通信任务
    TASK_PRIORITY_MQTT_MANAGER = 4,         // MQTT通信任务
    TASK_PRIORITY_MQTT_LOG = 4,             // MQTT LOG任务

    TASK_PRIORITY_WIFI_MANAGER = 3 // WiFi管理任务
} task_priority_t;
