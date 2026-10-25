/*
 * main.c
 *
 * v2 编解码器最小演示：打包一帧 → 逐字节解析 → 打印字段。
 */
#include <stdio.h>
#include <stdlib.h>
#include "pkt_protocol.h"
#include "pkt_protocol_buf.h"

static void demo_callback(uint8_t type, uint8_t seq, uint8_t flags,
                          const uint8_t* data, uint16_t len)
{
    (void)data;
    printf("[recv] type=0x%02X seq=%u flags=0x%02X len=%u\n",
           type, seq, flags, len);
}

int main(void)
{
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};

    uint16_t frame_len = 0;
    uint8_t* frame = protocol_pack_frame_ex(PROTOCOL_TYPE_SENSOR, 1,
                                            FRAME_FLAG_ACK_REQ, payload,
                                            sizeof(payload), &frame_len);
    if (frame == NULL)
    {
        printf("pack failed\n");
        return 1;
    }

    printf("Frame (%u bytes): ", frame_len);
    print_hex_data(frame, frame_len);

    protocol_receiver receiver;
    protocol_receiver_init(&receiver, 256, demo_callback);
    protocol_receiver_append(&receiver, frame, frame_len);
    protocol_receiver_destroy(&receiver);

    free(frame);
    return 0;
}
