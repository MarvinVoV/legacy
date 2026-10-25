/*
 * pkt_codec.h（host 测试 shim）
 *
 * ESP32 的 bridge_codec.c 引用 "pkt_codec.h" 的 PKT_MAX_PAYLOAD。
 * host 侧的协议头是 pkt_protocol.h（常量名 PROTOCOL_MAX_PAYLOAD）。
 * 本 shim 仅用于让 bridge_codec.c 在 host 原样编译（保持与 ESP32 源码一致）。
 */
#ifndef SERIAL_PKT_PROTOCOL_PKT_CODEC_SHIM_H
#define SERIAL_PKT_PROTOCOL_PKT_CODEC_SHIM_H

#include "pkt_protocol.h"

#ifndef PKT_MAX_PAYLOAD
#define PKT_MAX_PAYLOAD PROTOCOL_MAX_PAYLOAD
#endif
#ifndef PKT_FRAME_OVERHEAD
#define PKT_FRAME_OVERHEAD 8 /* magic(2)+type(1)+seq(1)+flags(1)+len(1)+crc(2) */
#endif
#ifndef PKT_MAX_FRAME_SIZE
#define PKT_MAX_FRAME_SIZE (PKT_FRAME_OVERHEAD + PKT_MAX_PAYLOAD)
#endif

#endif /* SERIAL_PKT_PROTOCOL_PKT_CODEC_SHIM_H */
