/**
 * @file ack_manager.h
 * @brief ACK 重传表（纯 C，无 FreeRTOS 依赖，可 host 单测）
 *
 * 设计：本组件只管"表 + 时序决策"，不做加锁也不直接调 UART 发送。
 *   - 线程安全由调用方（bridge）用自有的 mutex 包裹 ackmgr_* 调用实现；
 *   - 重传发送由调用方通过 resend 回调注入（解耦 uart_service）。
 * 这样表逻辑可在 host 上用 ctest 回归。
 *
 * 依赖 pkt_codec.h 的 PKT_MAX_FRAME_SIZE（host 侧有 shim）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pkt_codec.h"

#ifndef ACK_MGR_MAX_PENDING
#define ACK_MGR_MAX_PENDING 4
#endif

typedef struct
{
    bool active;
    uint8_t seq;
    uint8_t frame[PKT_MAX_FRAME_SIZE];
    size_t frame_len;
    uint32_t send_time_ms;
    uint8_t retries;
} ack_slot_t;

typedef struct
{
    ack_slot_t slots[ACK_MGR_MAX_PENDING];
    uint8_t next_seq;
} ack_manager_t;

typedef void (*ackmgr_resend_fn)(const uint8_t* frame, size_t len, void* ctx);

/** 初始化（清空表，next_seq=0） */
void ackmgr_init(ack_manager_t* m);

/** 分配一个当前未在途的 seq（0..255 回绕，跳过 active 槽位以避免碰撞） */
uint8_t ackmgr_alloc_seq(ack_manager_t* m);

/** 存入已编码帧用于 ACK 跟踪。返回 false：seq 已在途（碰撞）或表满。 */
bool ackmgr_add(ack_manager_t* m, uint8_t seq, const uint8_t* frame,
                size_t len, uint32_t now_ms);

/** 收到 ACK：清除对应 seq 的在途槽位 */
void ackmgr_clear(ack_manager_t* m, uint8_t seq);

/** 扫描超时：对每个超时槽位，未达 max_retries 则调 resend 并更新 send_time，
 *  达到 max_retries 则标记丢失（active=false）。返回本次新标记丢失的条数。 */
uint32_t ackmgr_scan(ack_manager_t* m, uint32_t now_ms, uint32_t timeout_ms,
                      uint8_t max_retries, ackmgr_resend_fn resend, void* ctx);
