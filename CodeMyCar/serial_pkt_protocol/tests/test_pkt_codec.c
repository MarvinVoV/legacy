/*
 * test_pkt_codec.c — v2 编解码器 host 测试
 *
 * 重点覆盖 uart_service 的接收契约：
 * 一次 UART 读取可能包含多帧，pkt_parser_feed_stream 必须派发每一帧。
 * 测试调用的是 firmware 同一个函数，而不是重新实现一份循环。
 */

#include <stdio.h>
#include <string.h>

#include "pkt_codec.h"

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); failures++; } \
                              else { printf("ok: %s\n", msg); } } while (0)

typedef struct {
    int count;
    uint8_t seqs[8];
} collector_t;

static void collect_cb(const pkt_frame_t* frame, void* ctx)
{
    collector_t* c = (collector_t*)ctx;
    if (c->count < 8)
    {
        c->seqs[c->count] = frame->seq;
    }
    c->count++;
}

static size_t encode_frame(pkt_frame_t* frame, uint8_t* out, size_t out_size)
{
    size_t len = 0;
    if (pkt_encode(frame, out, out_size, &len) != ESP_OK)
    {
        return 0;
    }
    return len;
}

int main(void)
{
    uint8_t buf[PKT_MAX_FRAME_SIZE * 2];
    pkt_parser_t parser;
    collector_t got;

    /* 1. 单帧 round-trip */
    pkt_frame_t f = {
        .type = PKT_TYPE_SENSOR, .seq = 7, .flags = 0,
        .payload_len = 3, .payload = {0x11, 0x22, 0x33},
    };
    size_t len = encode_frame(&f, buf, sizeof(buf));
    CHECK(len == PKT_FRAME_OVERHEAD + 3, "single frame length == 11");
    CHECK(buf[0] == 0x55 && buf[1] == 0xAA, "magic bytes 55 AA");

    pkt_parser_init(&parser);
    memset(&got, 0, sizeof(got));
    int n = pkt_parser_feed_stream(&parser, buf, len, collect_cb, &got);
    CHECK(n == 1 && got.count == 1, "single frame dispatched once");
    CHECK(got.count == 1 && got.seqs[0] == 7, "single frame seq=7");
    CHECK(parser.frame.payload_len == 3 &&
          memcmp(parser.frame.payload, f.payload, 3) == 0, "payload round-trip");

    /* 2. 一个 chunk 里两帧：feed_stream 必须回调两次（uart_service 修复点） */
    pkt_frame_t a = {
        .type = PKT_TYPE_ACK, .seq = 1, .flags = PKT_FLAG_ACK_RSP,
        .payload_len = 1, .payload = {0x05},
    };
    pkt_frame_t b = {
        .type = PKT_TYPE_SENSOR, .seq = 2, .flags = 0,
        .payload_len = 3, .payload = {0xAA, 0xBB, 0xCC},
    };
    size_t la = encode_frame(&a, buf, sizeof(buf));
    size_t lb = encode_frame(&b, buf + la, sizeof(buf) - la);
    CHECK(la > 0 && lb > 0, "two frames encoded");

    pkt_parser_init(&parser);
    memset(&got, 0, sizeof(got));
    n = pkt_parser_feed_stream(&parser, buf, la + lb, collect_cb, &got);
    CHECK(n == 2 && got.count == 2, "two frames in one chunk -> two dispatches");
    CHECK(got.count == 2 && got.seqs[0] == 1 && got.seqs[1] == 2,
          "dispatch order seq 1,2");

    /* 3. 分片：同一帧跨两次 chunk 到达，仍只派发一次 */
    pkt_parser_init(&parser);
    memset(&got, 0, sizeof(got));
    pkt_parser_feed_stream(&parser, buf, la - 2, collect_cb, &got);
    CHECK(got.count == 0, "partial frame dispatches nothing");
    pkt_parser_feed_stream(&parser, buf + la - 2, (la + lb) - (la - 2),
                           collect_cb, &got);
    CHECK(got.count == 2, "frames complete after second chunk");
    CHECK(got.seqs[0] == 1 && got.seqs[1] == 2, "split chunk seq order");

    /* 4. CRC 错误：第一帧丢弃，第二帧仍能解析 */
    uint8_t corrupt[PKT_MAX_FRAME_SIZE * 2];
    memcpy(corrupt, buf, la + lb);
    corrupt[7] ^= 0xFF; /* 改第一帧 payload（offset 7 为 payload 首字节） */
    pkt_parser_init(&parser);
    memset(&got, 0, sizeof(got));
    n = pkt_parser_feed_stream(&parser, corrupt, la + lb, collect_cb, &got);
    CHECK(n == 1 && got.count == 1, "corrupt frame dropped, following parsed");
    CHECK(got.count == 1 && got.seqs[0] == 2, "surviving frame is seq=2");

    /* 5. payload 上限 */
    pkt_frame_t maxf;
    memset(&maxf, 0, sizeof(maxf));
    maxf.type = PKT_TYPE_LOG;
    maxf.payload_len = PKT_MAX_PAYLOAD;
    len = encode_frame(&maxf, buf, sizeof(buf));
    CHECK(len == PKT_MAX_FRAME_SIZE, "max payload encodes to 128 bytes");

    pkt_frame_t toobig = maxf;
    toobig.payload_len = PKT_MAX_PAYLOAD + 1;
    size_t dummy = 0;
    CHECK(pkt_encode(&toobig, buf, sizeof(buf), &dummy) == ESP_ERR_INVALID_SIZE,
          "payload > 120 rejected");

    /* 6. 零 payload 帧 */
    pkt_frame_t zero = {.type = PKT_TYPE_HEARTBEAT, .seq = 9, .payload_len = 0};
    len = encode_frame(&zero, buf, sizeof(buf));
    CHECK(len == PKT_FRAME_OVERHEAD, "zero-payload frame length == 8");
    pkt_parser_init(&parser);
    memset(&got, 0, sizeof(got));
    n = pkt_parser_feed_stream(&parser, buf, len, collect_cb, &got);
    CHECK(n == 1 && got.seqs[0] == 9, "zero-payload round-trip");

    /* 7. LEN > 120：必须拒绝，并且解析器能恢复 */
    {
        uint8_t oversize[6 + 121 + 2];
        const uint8_t hdr[] = {0x55, 0xAA, 0x02, 0x00, 0x00, 121};
        memcpy(oversize, hdr, sizeof(hdr));
        memset(oversize + 6, 0x00, 121);
        oversize[127] = 0x00;
        oversize[128] = 0x00;

        pkt_parser_init(&parser);
        memset(&got, 0, sizeof(got));
        n = pkt_parser_feed_stream(&parser, oversize, sizeof(oversize), collect_cb, &got);
        CHECK(n == 0, "LEN=121 rejected");

        uint8_t good[PKT_MAX_FRAME_SIZE];
        pkt_frame_t ok = {.type = PKT_TYPE_LOG, .seq = 3, .payload_len = 2,
                          .payload = {0xDE, 0xAD}};
        size_t glen = encode_frame(&ok, good, sizeof(good));
        pkt_parser_feed_stream(&parser, good, glen, collect_cb, &got);
        CHECK(got.count == 1 && got.seqs[0] == 3, "parser recovers after LEN>120");
    }

    /* 8. LEN=255（最大字节值）也不能越界 */
    {
        uint8_t oversize[6 + 255 + 2];
        const uint8_t hdr[] = {0x55, 0xAA, 0x02, 0x00, 0x00, 255};
        memcpy(oversize, hdr, sizeof(hdr));
        memset(oversize + 6, 0x11, 255);
        oversize[261] = 0x00;
        oversize[262] = 0x00;

        pkt_parser_init(&parser);
        memset(&got, 0, sizeof(got));
        n = pkt_parser_feed_stream(&parser, oversize, sizeof(oversize), collect_cb, &got);
        CHECK(n == 0, "LEN=255 rejected");
    }

    /* 9. 最大合法 payload=120 的帧可以正确解析 */
    {
        pkt_frame_t big;
        memset(&big, 0, sizeof(big));
        big.type = PKT_TYPE_LOG;
        big.seq = 4;
        big.payload_len = PKT_MAX_PAYLOAD;
        memset(big.payload, 0x5A, sizeof(big.payload));
        uint8_t buf120[PKT_MAX_FRAME_SIZE];
        size_t len120 = encode_frame(&big, buf120, sizeof(buf120));
        pkt_parser_init(&parser);
        memset(&got, 0, sizeof(got));
        n = pkt_parser_feed_stream(&parser, buf120, len120, collect_cb, &got);
        CHECK(n == 1 && got.seqs[0] == 4, "max payload=120 parses");
        CHECK(parser.frame.payload_len == PKT_MAX_PAYLOAD, "max payload length kept");
    }

    /* 10. 两帧最大 payload 连发（256 字节 chunk） */
    {
        pkt_frame_t f1, f2;
        memset(&f1, 0, sizeof(f1)); memset(&f2, 0, sizeof(f2));
        f1.type = f2.type = PKT_TYPE_SENSOR;
        f1.seq = 10; f2.seq = 11;
        f1.payload_len = f2.payload_len = PKT_MAX_PAYLOAD;
        uint8_t two[PKT_MAX_FRAME_SIZE * 2];
        size_t l1 = encode_frame(&f1, two, sizeof(two));
        size_t l2 = encode_frame(&f2, two + l1, sizeof(two) - l1);
        pkt_parser_init(&parser);
        memset(&got, 0, sizeof(got));
        n = pkt_parser_feed_stream(&parser, two, l1 + l2, collect_cb, &got);
        CHECK(n == 2 && got.seqs[0] == 10 && got.seqs[1] == 11,
              "two max-size frames in one chunk");
    }

    /* 11. 4KB 垃圾流 + 末尾合法帧：不崩溃、不越界、只派发 1 帧 */
    {
        static uint8_t junk[4096 + PKT_MAX_FRAME_SIZE];
        memset(junk, 0x00, sizeof(junk));
        uint8_t good[PKT_MAX_FRAME_SIZE];
        pkt_frame_t ok = {.type = PKT_TYPE_LOG, .seq = 12, .payload_len = 1,
                          .payload = {0x7E}};
        size_t glen = encode_frame(&ok, good, sizeof(good));
        memcpy(junk + 4096, good, glen);

        pkt_parser_init(&parser);
        memset(&got, 0, sizeof(got));
        for (size_t off = 0; off < sizeof(junk); off += 256)
        {
            size_t chunk = sizeof(junk) - off;
            if (chunk > 256) chunk = 256;
            pkt_parser_feed_stream(&parser, junk + off, chunk, collect_cb, &got);
        }
        CHECK(got.count == 1 && got.seqs[0] == 12,
              "4KB junk then valid frame -> exactly one dispatch");
    }

    printf("\n%s (%d failures)\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
    return failures ? 1 : 0;
}
