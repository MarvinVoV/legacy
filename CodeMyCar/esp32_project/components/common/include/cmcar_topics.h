#pragma once

/**
 * @file cmcar_topics.h
 * @brief MQTT Topic 常量（统一契约，见 docs/mqtt-topics.md）
 *
 * 原则：topic 只含单一 device_id 层级；数据来源（esp32/stm32）放进 JSON payload
 * 的 origin 字段，不进 topic。
 */

#include "cmcar_types.h"

/* Topic 前缀 */
#define CMCAR_TOPIC_PREFIX "codemycar"

/* ============== 上行（ESP32 → Broker） ============== */
#define CMCAR_TOPIC_SENSOR    "data/" CMCAR_DEVICE_ID "/sensor"
#define CMCAR_TOPIC_LOG       "data/" CMCAR_DEVICE_ID "/log"
#define CMCAR_TOPIC_HEARTBEAT "data/" CMCAR_DEVICE_ID "/heartbeat"
#define CMCAR_TOPIC_STATUS    "data/" CMCAR_DEVICE_ID "/status"
#define CMCAR_TOPIC_ACK       "data/" CMCAR_DEVICE_ID "/ack"

/* ============== 下行（Broker → ESP32） ============== */
/* 订阅自身 device_id 的命令（不用 + 通配，避免收到他人命令） */
#define CMCAR_TOPIC_CMD_SUB "cmd/" CMCAR_DEVICE_ID "/exec"
