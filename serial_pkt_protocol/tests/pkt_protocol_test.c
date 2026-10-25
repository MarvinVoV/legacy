/*
 * pkt_protocol_test.c
 *
 * v2 编解码器单元测试（Unity）：
 *   - round-trip 打包/解析
 *   - 分片重组
 *   - CRC 失败丢弃
 *   - 超长 payload 拒绝
 *   - 非法 LEN 字段拒绝
 *   - SEQ 回绕
 *   - ACK 管理器（add/check/超时过期）
 *   - 多帧连发 + 帧间垃圾
 *   - 零 payload 帧
 *   - MQTT topic 通配匹配
 */
#include "unity.h"
#include "pkt_protocol.h"
#include "pkt_protocol_buf.h"
#include "mqtt_utils.h"

#include <string.h>

/* ==================== 测试上下文 ==================== */

static uint8_t  cb_type;
static uint8_t  cb_seq;
static uint8_t  cb_flags;
static uint16_t cb_len;
static uint8_t  cb_payload[PROTOCOL_MAX_PAYLOAD];
static int      cb_count;

static protocol_receiver receiver;

static void mock_callback(uint8_t type, uint8_t seq, uint8_t flags,
                          const uint8_t* data, uint16_t len)
{
    cb_type  = type;
    cb_seq   = seq;
    cb_flags = flags;
    cb_len   = len;
    if (len > 0 && data != NULL)
    {
        memcpy(cb_payload, data, len);
    }
    cb_count++;
}

void setUp(void)
{
    cb_count = 0;
    cb_type = cb_seq = cb_flags = 0;
    cb_len = 0xFFFF;
    memset(cb_payload, 0, sizeof(cb_payload));
    protocol_receiver_init(&receiver, 256, mock_callback);
}

void tearDown(void)
{
    protocol_receiver_destroy(&receiver);
}

/* ==================== 用例 ==================== */

/* 1. round-trip：打包 → 逐字节解析，字段与 payload 全对 */
void test_pack_round_trip(void)
{
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    uint16_t frame_len = 0;
    uint8_t* frame = protocol_pack_frame_ex(PROTOCOL_TYPE_CONTROL, 42,
                                            FRAME_FLAG_ACK_REQ, payload,
                                            sizeof(payload), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_EQUAL(8 + sizeof(payload), frame_len);

    protocol_receiver_append(&receiver, frame, frame_len);
    free(frame);

    TEST_ASSERT_EQUAL_INT(1, cb_count);
    TEST_ASSERT_EQUAL_UINT8(PROTOCOL_TYPE_CONTROL, cb_type);
    TEST_ASSERT_EQUAL_UINT8(42, cb_seq);
    TEST_ASSERT_EQUAL_UINT8(FRAME_FLAG_ACK_REQ, cb_flags);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), cb_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, cb_payload, sizeof(payload));
}

/* 2. 分片重组：一帧拆成三段 append */
void test_partial_reassembly(void)
{
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint16_t frame_len = 0;
    uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, payload,
                                         sizeof(payload), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);

    const uint16_t cut1 = 3;
    const uint16_t cut2 = 7;
    protocol_receiver_append(&receiver, frame, cut1);
    protocol_receiver_append(&receiver, frame + cut1, cut2 - cut1);
    protocol_receiver_append(&receiver, frame + cut2, frame_len - cut2);
    free(frame);

    TEST_ASSERT_EQUAL_INT(1, cb_count);
    TEST_ASSERT_EQUAL_UINT8(PROTOCOL_TYPE_SENSOR, cb_type);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), cb_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, cb_payload, sizeof(payload));
}

/* 3. CRC 失败：翻转 payload 一字节，不应产生回调 */
void test_crc_failure_discarded(void)
{
    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    uint16_t frame_len = 0;
    uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_LOG, payload,
                                         sizeof(payload), &frame_len);
    TEST_ASSERT_NOT_NULL(frame);
    frame[8] ^= 0xFF; /* 翻转 payload 首字节，CRC 失配 */

    protocol_receiver_append(&receiver, frame, frame_len);
    free(frame);

    TEST_ASSERT_EQUAL_INT(0, cb_count);
}

/* 4. 超长 payload 拒绝：pack_frame_ex 对 >120 返回 NULL；encode 返回 -2 */
void test_oversized_payload_rejected(void)
{
    uint8_t big[PROTOCOL_MAX_PAYLOAD + 1] = {0};
    uint16_t frame_len = 0;
    uint8_t* frame = protocol_pack_frame_ex(PROTOCOL_TYPE_CONTROL, 0, 0,
                                            big, sizeof(big), &frame_len);
    TEST_ASSERT_NULL(frame);

    protocol_frame_t f = { .type = PROTOCOL_TYPE_CONTROL,
                           .payload_len = PROTOCOL_MAX_PAYLOAD + 1 };
    uint8_t buf[PROTOCOL_MAX_FRAME_SIZE];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(-2, protocol_encode(&f, buf, sizeof(buf), &out_len));
}

/* 5. 非法 LEN 字段：手构一帧 LEN=200，parser 应 reset 无回调 */
void test_illegal_len_field_rejected(void)
{
    /* 55 AA | TYPE=01 | SEQ=00 | FLAGS=00 | LEN=C8(200) | ... */
    const uint8_t bad[] = {0x55, 0xAA, 0x01, 0x00, 0x00, 0xC8,
                           0x00, 0x00, 0x00, 0x00};
    protocol_receiver_append(&receiver, bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(0, cb_count);
}

/* 6. SEQ 回绕：next_seq 0..255 后回 0 */
void test_seq_wraparound(void)
{
    protocol_ack_mgr_t mgr;
    protocol_ack_mgr_init(&mgr);
    TEST_ASSERT_EQUAL_UINT8(0, protocol_ack_mgr_next_seq(&mgr));
    for (int i = 0; i < 254; i++)  /* 再取 254 个：1..254 */
    {
        (void)protocol_ack_mgr_next_seq(&mgr);
    }
    TEST_ASSERT_EQUAL_UINT8(255, protocol_ack_mgr_next_seq(&mgr)); /* 第 256 个 */
    TEST_ASSERT_EQUAL_UINT8(0, protocol_ack_mgr_next_seq(&mgr));  /* 回绕 */
}

/* 7. ACK 管理器：add 满 4 槽、第 5 个失败、check 命中移除、超时 3 次过期、晚加入的不过期 */
void test_ack_mgr_add_check_timeout(void)
{
    protocol_ack_mgr_t mgr;
    protocol_ack_mgr_init(&mgr);

    /* 7a 满槽 + check 移除 + 复用空槽 */
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 10, 0));
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 20, 0));
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 30, 0));
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 40, 0));
    TEST_ASSERT_EQUAL_INT(-1, protocol_ack_mgr_add(&mgr, 50, 0)); /* 满 */
    TEST_ASSERT_EQUAL_INT(1, protocol_ack_mgr_check(&mgr, 20));   /* 命中移除 */
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_check(&mgr, 20));   /* 已移除 */
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 60, 0));  /* 复用空槽 */

    /* 7b 选择性超时：seq=10 需 3 次 200ms 才过期；晚加入的 99 不过期 */
    protocol_ack_mgr_init(&mgr);
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 10, 0));
    uint8_t expired[4] = {0};
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_check_timeout(&mgr, 200, expired, 4));
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_check_timeout(&mgr, 400, expired, 4));
    TEST_ASSERT_EQUAL_INT(0, protocol_ack_mgr_add(&mgr, 99, 600)); /* 晚加入 */
    TEST_ASSERT_EQUAL_INT(1, protocol_ack_mgr_check_timeout(&mgr, 600, expired, 4));
    TEST_ASSERT_EQUAL_UINT8(10, expired[0]);
    TEST_ASSERT_EQUAL_INT(1, protocol_ack_mgr_check(&mgr, 99)); /* 99 仍 pending */
}

/* 8. 多帧连发：一次 append 两帧，应两次回调 */
void test_multiple_frames_one_buf(void)
{
    const uint8_t p1[] = {0xA1, 0xA2};
    const uint8_t p2[] = {0xB1, 0xB2, 0xB3};
    uint16_t l1 = 0, l2 = 0;
    uint8_t* f1 = protocol_pack_frame_ex(PROTOCOL_TYPE_SENSOR, 1, 0, p1, sizeof(p1), &l1);
    uint8_t* f2 = protocol_pack_frame_ex(PROTOCOL_TYPE_SENSOR, 2, 0, p2, sizeof(p2), &l2);
    TEST_ASSERT_NOT_NULL(f1);
    TEST_ASSERT_NOT_NULL(f2);

    uint8_t buf[256];
    memcpy(buf, f1, l1);
    memcpy(buf + l1, f2, l2);
    free(f1);
    free(f2);

    protocol_receiver_append(&receiver, buf, l1 + l2);
    TEST_ASSERT_EQUAL_INT(2, cb_count);
    TEST_ASSERT_EQUAL_UINT8(2, cb_seq);         /* 最后一帧 seq=2 */
    TEST_ASSERT_EQUAL_UINT16(3, cb_len);
}

/* 9. 帧间垃圾：两帧之间塞无效字节，仍解析出两帧 */
void test_garbage_between_frames(void)
{
    const uint8_t p[] = {0x01};
    uint16_t l = 0;
    uint8_t* f = protocol_pack_frame(PROTOCOL_TYPE_HEARTBEAT, p, sizeof(p), &l);
    TEST_ASSERT_NOT_NULL(f);

    uint8_t buf[256];
    size_t pos = 0;
    memcpy(buf + pos, f, l); pos += l;
    buf[pos++] = 0x00;  /* 垃圾 */
    buf[pos++] = 0x11;
    memcpy(buf + pos, f, l); pos += l;
    free(f);

    protocol_receiver_append(&receiver, buf, pos);
    TEST_ASSERT_EQUAL_INT(2, cb_count);
}

/* 10. 零 payload 帧 */
void test_zero_payload_frame(void)
{
    uint16_t l = 0;
    uint8_t* f = protocol_pack_frame(PROTOCOL_TYPE_ACK, NULL, 0, &l);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(8, l); /* 仅 overhead */

    protocol_receiver_append(&receiver, f, l);
    free(f);
    TEST_ASSERT_EQUAL_INT(1, cb_count);
    TEST_ASSERT_EQUAL_UINT16(0, cb_len);
    TEST_ASSERT_EQUAL_UINT8(PROTOCOL_TYPE_ACK, cb_type);
}

/* 11. MQTT topic 通配匹配 */
void test_mqtt_topic_match(void)
{
    TEST_ASSERT_TRUE(mqtt_topic_match("cmd/+/exec", "cmd/DEV1/exec"));
    TEST_ASSERT_TRUE(mqtt_topic_match("data/+/sensor", "data/DEV1/sensor"));
    TEST_ASSERT_FALSE(mqtt_topic_match("data/+/sensor", "data/esp32/DEV1/sensor"));
    TEST_ASSERT_FALSE(mqtt_topic_match("cmd/esp32/+/exec", "cmd/DEV1/exec"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pack_round_trip);
    RUN_TEST(test_partial_reassembly);
    RUN_TEST(test_crc_failure_discarded);
    RUN_TEST(test_oversized_payload_rejected);
    RUN_TEST(test_illegal_len_field_rejected);
    RUN_TEST(test_seq_wraparound);
    RUN_TEST(test_ack_mgr_add_check_timeout);
    RUN_TEST(test_multiple_frames_one_buf);
    RUN_TEST(test_garbage_between_frames);
    RUN_TEST(test_zero_payload_frame);
    RUN_TEST(test_mqtt_topic_match);
    return UNITY_END();
}
