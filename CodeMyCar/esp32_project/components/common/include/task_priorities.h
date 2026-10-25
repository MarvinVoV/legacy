/**
 * @brief 任务优先级统一管理（v2 组件布局）
 *
 * 数值越大优先级越高。原则：ISR 必须尽快排空 → UART RX 最高；
 * 命令下行优先于遥测上行；MQTT/日志次之；心跳/看门狗最低但仍被调度。
 */

#pragma once

typedef enum
{
    TASK_PRIO_IDLE = 0,

    TASK_PRIO_EVENT = 9,        /* esp_event 默认事件循环任务（保留最高） */

    /* UART：必须高于一切消费者，避免 ISR 事件队列溢出 */
    TASK_PRIO_UART_RX = 8,     /* UART RX：排空驱动事件队列 → RingBuf → 解析 */
    TASK_PRIO_UART_TX = 7,     /* UART TX：tx_queue → uart_write_bytes */

    /* 桥接：命令下行（时序敏感）优先于遥测上行 */
    TASK_PRIO_BRIDGE_DOWNLINK = 6, /* MQTT JSON → 二进制 → UART */
    TASK_PRIO_BRIDGE_UPLINK = 5,   /* UART 帧 → JSON → MQTT publish */

    TASK_PRIO_REMOTE_LOG = 5,  /* 远程日志发布（与上行同级） */
    TASK_PRIO_MQTT = 4,        /* MQTT 客户端任务（esp_mqtt） */
    TASK_PRIO_HEALTH = 3,      /* 心跳 / 看门狗喂狗（最低，但需被调度） */
} task_priority_t;
