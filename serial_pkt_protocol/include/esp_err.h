#pragma once

/*
 * esp_err.h（host 测试 shim）
 *
 * 让 ESP32 的 pkt_codec.c/bridge_codec.c 能在 host 上原样编译。
 * 只定义协议层用到的错误码，值与 ESP-IDF 保持一致。
 */

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
