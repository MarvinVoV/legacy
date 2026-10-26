#pragma once

/**
 * @file bridge_codec.h
 * @brief JSON ↔ Binary 转换（见 docs/cmd-mapping.md）
 *
 * 下行：pyserver JSON ControlCommand → STM32 9 字节 ControlCmd payload
 * 上行：STM32 二进制 sensor_data_t / log_entry_t → pyserver JSON
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ControlCmd wire payload 大小（显式宽度，见 docs/cmd-mapping.md §3.2） */
#define BRIDGE_CONTROL_CMD_SIZE 9

/* 运动模式（与 STM32 motion_common.h 对齐） */
enum
{
    BRIDGE_MODE_EMERGENCY_STOP = 0,
    BRIDGE_MODE_DIRECT        = 1,
    BRIDGE_MODE_DIFFERENTIAL  = 2,
    BRIDGE_MODE_DRIVE         = 3,
    BRIDGE_MODE_MAX           = 4,
};

/**
 * @brief 下行：JSON ControlCommand → 9 字节 ControlCmd
 * @param json       JSON 文本
 * @param len        JSON 长度
 * @param out        输出缓冲区（至少 BRIDGE_CONTROL_CMD_SIZE 字节）
 * @param out_seq    [out] 命令序列号（映射到帧头 seq，用于 ACK）
 * @param out_ack    [out] 是否请求 ACK（运动命令默认 true）
 * @return 13 成功，0 失败（解析错误/未知模式/必填字段缺失）
 */
int bridge_json_to_control_cmd(const char* json, size_t len,
                               uint8_t* out, uint8_t* out_seq, bool* out_ack);

/**
 * @brief 上行：二进制 sensor_data_t → JSON（匹配 pyserver SensorData）
 * @return JSON 字符串长度（不含\0），0 失败
 */
int bridge_sensor_to_json(const uint8_t* payload, uint8_t len,
                          char* out, size_t out_size);

/**
 * @brief 上行：二进制 log_entry_t → JSON（匹配 pyserver LogEntry）
 * @return JSON 字符串长度（不含\0），0 失败
 */
int bridge_log_to_json(const uint8_t* payload, uint8_t len,
                       char* out, size_t out_size);

/**
 * @brief 下行：判断是否为 ping 命令并取 seq
 * @param out_seq [out] seq（& 0xFF）；非 ping 时不修改
 * @return true 是 ping，false 不是/解析失败
 */
bool bridge_json_is_ping(const char* json, size_t len, uint8_t* out_seq);

/**
 * @brief 构建 pong JSON：{"seq":N,"pong":true,"uptime_s":U,"heap":H}
 * @return 长度（不含 \0）；缓冲区不足返回 0
 */
int bridge_pong_to_json(uint8_t seq, unsigned long long uptime_s,
                        unsigned long heap, char* out, size_t out_size);
