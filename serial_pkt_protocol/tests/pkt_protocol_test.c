//
// Created by marvin on 2025/2/15.
//
#include "unity.h"
#include "pkt_protocol.h"
#include "pkt_protocol_buf.h"
#include "mqtt_utils.h"
#include "ctrl_protocol.h"
#include <string.h>

static int callback_triggered = 0;

static void mock_callback(protocol_type_t type, uint8_t* data, uint16_t len)
{
    callback_triggered++;

    // control_cmd_t* cmd = (control_cmd_t*)data;
    // print_hex_data(data, sizeof(control_cmd_t));
}

// 测试上下文
protocol_receiver receiver;


void setUp(void)
{
    protocol_receiver_init(&receiver, 100, (frame_callback)mock_callback);
    callback_triggered = 0;
}

void tearDown(void)
{
    protocol_receiver_destroy(&receiver);
}

void test_all_append(void)
{
    // const uint8_t data[] = {
    //     0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55
    //
    // };
    const uint8_t data[] = {
        0x55, 0xAA, 0x02, 0x0D, 0x00, 0x00, 0x03, 0x0A,
        0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x57, 0x6F, 0x72,
        0x6C, 0x64, 0x49, 0x08, 0xAA, 0x55
    };
    protocol_receiver_append(&receiver, data, sizeof(data));
    TEST_ASSERT_EQUAL(1, callback_triggered); // 验证回调触发
}

void test_ctrl_protocol(void)
{

    const char *message = "helloworld";  // 10字符
    const uint8_t message_len = strlen(message);

    const size_t total_size = sizeof(control_cmd_t) + message_len;
    control_cmd_t *cmd = malloc(total_size);
    // 结构体初始化 ----------
    memset(cmd, 0, total_size);  // 清零防止脏数据
    // 基础字段
    cmd->ctrl_id = 0x01;  // 示例设备ID
    cmd->ctrl_fields = CTRL_FIELD_TEXT;  // 设置文本控制位
    cmd->ping_text.len = message_len;
    memcpy(cmd->ping_text.msg, message, message_len);  // 注意不拷贝终止符

    // 转换为字节流 ----------
    uint8_t *byte_stream = (uint8_t*)cmd;


    uint16_t frame_len;
    const uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_CONTROL, byte_stream,
                                               total_size, &frame_len);
    print_hex_data(frame, frame_len);
    TEST_ASSERT_NOT_NULL(frame);

    // Append
    protocol_receiver_append(&receiver, frame, frame_len);
    TEST_ASSERT_EQUAL(1, callback_triggered); // 验证回调触发
}

void test_partial_append(void)
{
    const uint8_t part1[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    const uint8_t part2[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};
    protocol_receiver_append(&receiver, part1, sizeof(part1));
    protocol_receiver_append(&receiver, part2, sizeof(part2));
    TEST_ASSERT_EQUAL(1, callback_triggered); // 验证回调触发
}

void test_multiple_partial_append(void)
{
    // invalid
    uint8_t part1[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t part2[] = {0x05, 0x06, 0x07, 0x08, 0x09};
    // valid frame
    uint8_t part3[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part4[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};

    protocol_receiver_append(&receiver, part1, sizeof(part1));
    protocol_receiver_append(&receiver, part2, sizeof(part2));
    protocol_receiver_append(&receiver, part3, sizeof(part3));
    protocol_receiver_append(&receiver, part4, sizeof(part4));

    TEST_ASSERT_EQUAL(1, callback_triggered);
    // 即丢弃无效帧数据
    TEST_ASSERT_EQUAL(0, receiver.write_pos);
}

void test_multiple_partial_append2(void)
{
    // valid frame
    uint8_t part1[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part2[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};
    // valid frame
    uint8_t part3[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part4[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};

    protocol_receiver_append(&receiver, part1, sizeof(part1));
    protocol_receiver_append(&receiver, part2, sizeof(part2));
    protocol_receiver_append(&receiver, part3, sizeof(part3));
    protocol_receiver_append(&receiver, part4, sizeof(part4));

    TEST_ASSERT_EQUAL(2, callback_triggered);
    // 即丢弃无效帧数据
    TEST_ASSERT_EQUAL(0, receiver.write_pos);
}

void test_multiple_partial_append3(void)
{
    // valid frame
    uint8_t part1[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part2[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};
    // valid frame
    uint8_t part3[] = {
        0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55
    };

    protocol_receiver_append(&receiver, part1, sizeof(part1));
    protocol_receiver_append(&receiver, part2, sizeof(part2));
    protocol_receiver_append(&receiver, part3, sizeof(part3));

    TEST_ASSERT_EQUAL(2, callback_triggered);
    // 即丢弃无效帧数据
    TEST_ASSERT_EQUAL(0, receiver.write_pos);
}

void test_multiple_partial_append4(void)
{
    // valid frame
    uint8_t part1[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part2[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};
    // valid frame
    uint8_t part3[] = {
        0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55
    };

    // invalid
    uint8_t part4[] = {0x01, 0x02, 0x03, 0x04};
    // valid frame
    uint8_t part5[] = {0x55, 0xAA, 0x01, 0x0A, 0x00, 0x68, 0x65, 0x6C, 0x6C};
    uint8_t part6[] = {0x6F, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x84, 0xDA, 0xAA, 0x55};

    protocol_receiver_append(&receiver, part1, sizeof(part1));
    protocol_receiver_append(&receiver, part2, sizeof(part2));
    protocol_receiver_append(&receiver, part3, sizeof(part3));
    protocol_receiver_append(&receiver, part4, sizeof(part4));
    protocol_receiver_append(&receiver, part5, sizeof(part5));
    protocol_receiver_append(&receiver, part6, sizeof(part6));

    TEST_ASSERT_EQUAL(3, callback_triggered);
    // 即丢弃无效帧数据
    TEST_ASSERT_EQUAL(0, receiver.write_pos);
}


// --- Good Cases ---

// 测试用例 1: 正常追加并解析完整帧
void test_append_normal(void)
{
    // Prepare data
    const uint8_t sensor_data[] = {0x01, 0x02, 0x03, 0x04};

    uint16_t frame_len;
    const uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, sensor_data,
                                               sizeof(sensor_data), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);

    // Append
    protocol_receiver_append(&receiver, frame, frame_len);

    TEST_ASSERT_EQUAL(1, callback_triggered); // 验证回调触发
    TEST_ASSERT_EQUAL(0, receiver.write_pos); // 验证缓冲区清空
}

// 测试用例 2: 移动已处理数据后追加新数据
void test_append_after_processing(void)
{
    // 模拟已处理部分数据
    receiver.processed_pos = 10;
    receiver.write_pos = 50;

    const uint8_t new_data[60] = {0}; // 填充测试数据
    protocol_receiver_append(&receiver, new_data, 60);

    // TEST_ASSERT_EQUAL(100, receiver.write_pos); // 验证缓冲区填满

    const uint8_t sensor_data[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t frame_len;
    const uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, sensor_data,
                                               sizeof(sensor_data), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);
    protocol_receiver_append(&receiver, frame, frame_len);
    TEST_ASSERT_EQUAL(0, receiver.processed_pos); // 验证处理位置重置
}

// --- Bad Cases ---
/**
 * 测试用例 3: 动态扩容失败且部分丢弃数据
 * NOTE: 需要手动mock内存申请失败
 */
void test_append_partial_discard_on_realloc_fail(void)
{
    protocol_receiver receiver;
    protocol_receiver_init(&receiver, 50, (frame_callback)mock_callback);

    // NOTE 需要手动mock内存申请失败
    receiver.write_pos = 40;          // 初始写入位置
    const uint8_t new_data[20] = {0}; // 追加 20字节数据
    protocol_receiver_append(&receiver, new_data, 20);

    TEST_ASSERT_EQUAL(50, receiver.write_pos); // 仅写入 10 字节
}

/**
 * 测试用例 4: 动态扩容失败且无空间，全部丢弃
 * NOTE: 需要手动mock内存申请失败
 */
void test_append_full_discard_on_realloc_fail(void)
{
    protocol_receiver receiver;
    protocol_receiver_init(&receiver, 50, (frame_callback)mock_callback);
    receiver.write_pos = 50; // 缓冲区已满

    const uint8_t new_data[20] = {0}; // 追加 20字节数据
    protocol_receiver_append(&receiver, new_data, 20);

    TEST_ASSERT_EQUAL(50, receiver.write_pos); // 无新数据写入
}

void test_htole16()
{
    printf("byte mode=%d\n",IS_LITTLE_ENDIAN);
    const uint16_t big_endian_value = 0x1234;
    const uint16_t little_endian_value = htole16(big_endian_value);
    printf("Big-Endian:    0x%04X\n", big_endian_value);
    printf("Little-Endian: 0x%04X\n", little_endian_value);
    TEST_ASSERT_EQUAL_HEX16(0x3412, little_endian_value);
}


void test_pack_and_parse()
{
    // Pack
    const uint8_t sensor_data[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t frame_len;
    const uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, sensor_data,
                                               sizeof(sensor_data), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);
    printf("Sensor Frame (%u bytes):\n", frame_len);
    print_hex_data(frame, frame_len);

    // Parse
    protocol_parser_t parser;
    protocol_parser_init(&parser);
    int result = 0;
    for (uint16_t i = 0; i < frame_len; i++)
    {
        if (protocol_parse_byte(&parser, frame[i]))
        {
            result = 1;
            print_hex_data(parser.frame.data, parser.frame.len);
            free(parser.frame.data);
            protocol_parser_init(&parser);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, result);
}

void test_mqtt_topic_match()
{
    bool res = mqtt_topic_match("cmd/esp32/+/exec", "cmd/esp32/DEV1/exec");
    TEST_ASSERT_TRUE(res);
    res = mqtt_topic_match("a/+/b", "a/test/b");
    TEST_ASSERT_TRUE(res);

    res = mqtt_topic_match("cmd/esp32/+/exec", "cmd/esp32/invalid/exec/extra");
    TEST_ASSERT_FALSE(res);
}

// --- 主函数运行所有测试 ---
int main(void)
{
    UNITY_BEGIN();
    // RUN_TEST(test_mqtt_topic_match);
    RUN_TEST(test_ctrl_protocol);

    // RUN_TEST(test_htole16);
    // RUN_TEST(test_all_append);
    // RUN_TEST(test_partial_append);
    // RUN_TEST(test_pack_and_parse);
    // RUN_TEST(test_append_normal);
    // RUN_TEST(test_append_after_processing);
    // RUN_TEST(test_multiple_partial_append);
    // RUN_TEST(test_multiple_partial_append2);
    // RUN_TEST(test_multiple_partial_append3);
    // RUN_TEST(test_multiple_partial_append4);
    // RUN_TEST(test_append_partial_discard_on_realloc_fail);
    // RUN_TEST(test_append_full_discard_on_realloc_fail);
    return UNITY_END();
}
