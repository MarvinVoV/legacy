#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "pkt_protocol.h"
#define FRAME_TAIL 0x55AA
// 初始化解析器
protocol_parser_t parser;

void process_received_data(uint8_t *buffer, uint16_t size) {
    printf("Received Data: ");
    for (uint16_t i = 0; i < size; i++) {
        printf("%02X ", buffer[i]);
    }
    printf("\n");
    for (uint16_t i = 0; i < size; i++) {
        if (protocol_parse_byte(&parser, buffer[i])) {
            printf("✅ 解析成功！\n");
            printf("   类型: %d\n", parser.frame.type);
            printf("   长度: %d\n", parser.frame.len);
            printf("   数据: ");
            for (int j = 0; j < parser.frame.len; j++) {
                printf("%02X ", parser.frame.data[j]);
            }
            printf("\n");

            free(parser.frame.data);
            protocol_parser_init(&parser);
        }
    }
}

int main(void) {
    // uint8_t test_data[] = {0x11, 0x22, 0x33, 0x44};
    // uint16_t frame_len;
    // uint8_t *packed_frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, test_data, sizeof(test_data), &frame_len);
    //
    // if (!packed_frame) {
    //     printf("❌ 数据打包失败！\n");
    //     return -1;
    // }
    //
    // protocol_parser_init(&parser);
    //
    // printf("📥 解析帧数据...\n");
    // process_received_data(packed_frame, frame_len);
    //
    // free(packed_frame);

    // uint16_t data = 0xFBAB;
    // uint8_t h = (data >> 8) & 0xFF;
    // uint8_t l = data & 0xFF;
    // printf("%02X %02X\n", h, l);
    printf("%lu", sizeof(uint16_t));

    return 0;
}
