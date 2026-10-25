#pragma once

/**
 * @file cmcar_types.h
 * @brief CodeMyCar 公共类型定义
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 设备标识（来自 Kconfig） */
#ifdef CONFIG_CMCAR_DEVICE_ID
#define CMCAR_DEVICE_ID CONFIG_CMCAR_DEVICE_ID
#else
#define CMCAR_DEVICE_ID "DEV_1"
#endif

/* 通用帧接收回调 */
typedef void (*cmcar_frame_cb_t)(uint8_t type, const uint8_t* data,
                                 uint16_t len, void* user_ctx);
