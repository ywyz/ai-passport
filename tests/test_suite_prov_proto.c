// tests/test_suite_prov_proto.c —— 配网分包帧协议主机测试。
// 覆盖:单包/多包切帧与重组、重复 seq、乱序、无 START 的中间包、
// 半条超时清空、超长拒绝、配对码错误计数与锁定。
#include <assert.h>
#include <string.h>

#include "suite_prov_proto.h"

static uint8_t s_msg_buf[SUITE_PROV_MSG_MAX];
static uint8_t s_frame_buf[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];

// 把一条消息按 mtu_payload 全部切帧并逐帧喂给重组器,返回最终结果长度。
static int roundtrip(const uint8_t *msg, size_t len, size_t mtu_payload,
                     suite_prov_reasm_t *r, uint32_t now_ms)
{
    size_t frames = suite_prov_frame_count(len, mtu_payload);
    assert(frames > 0);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    int ret = 0;
    for (size_t i = 0; i < frames; i++) {
        size_t flen = suite_prov_frame_build(msg, len, mtu_payload, i,
                                             s_frame_buf, sizeof(s_frame_buf));
        assert(flen >= SUITE_PROV_HEADER_LEN);
        ret = suite_prov_reasm_feed(r, s_frame_buf, flen, now_ms,
                                    &out, &out_len);
        if (i + 1 < frames) assert(ret == 0);
    }
    assert(ret == (int)len);
    assert(out_len == len);
    assert(memcmp(out, msg, len) == 0);
    return ret;
}

static void test_single_packet(void)
{
    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, s_msg_buf, sizeof(s_msg_buf));

    const char *m = "{\"hello\":1}";
    size_t frames = suite_prov_frame_count(strlen(m), SUITE_PROV_PAYLOAD_MAX);
    assert(frames == 1);
    size_t flen = suite_prov_frame_build((const uint8_t *)m, strlen(m),
                                         SUITE_PROV_PAYLOAD_MAX, 0,
                                         s_frame_buf, sizeof(s_frame_buf));
    assert(flen == strlen(m) + 2);
    assert(s_frame_buf[0] == SUITE_PROV_FLAG_SINGLE);
    assert(s_frame_buf[1] == 0);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    int ret = suite_prov_reasm_feed(&r, s_frame_buf, flen, 100, &out, &out_len);
    assert(ret == (int)strlen(m));
    assert(memcmp(out, m, strlen(m)) == 0);
}

static void test_multi_packet_flags_and_seq(void)
{
    // 用最小净荷 18 切一条 50 字节消息 → 3 帧(18/18/14)。
    uint8_t msg[50];
    for (size_t i = 0; i < sizeof(msg); i++) msg[i] = (uint8_t)('A' + i % 26);

    size_t frames = suite_prov_frame_count(sizeof(msg), 18);
    assert(frames == 3);

    size_t f0 = suite_prov_frame_build(msg, sizeof(msg), 18, 0,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(s_frame_buf[0] == SUITE_PROV_FLAG_START);
    assert(s_frame_buf[1] == 0);
    assert(f0 == 20);

    size_t f1 = suite_prov_frame_build(msg, sizeof(msg), 18, 1,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(s_frame_buf[0] == 0x00);
    assert(s_frame_buf[1] == 1);
    assert(f1 == 20);

    size_t f2 = suite_prov_frame_build(msg, sizeof(msg), 18, 2,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(s_frame_buf[0] == SUITE_PROV_FLAG_END);
    assert(s_frame_buf[1] == 2);
    assert(f2 == 16);

    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, s_msg_buf, sizeof(s_msg_buf));
    assert(roundtrip(msg, sizeof(msg), 18, &r, 0) == (int)sizeof(msg));
}

static void test_duplicate_seq_is_idempotent(void)
{
    uint8_t msg[40];
    memset(msg, 'X', sizeof(msg));

    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, s_msg_buf, sizeof(s_msg_buf));
    const uint8_t *out = NULL;
    size_t out_len = 0;

    size_t f0 = suite_prov_frame_build(msg, sizeof(msg), 18, 0,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f0, 0, &out, &out_len) == 0);
    // 首包重发:START 语义为重开会话,允许并重置(不是错误)。
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f0, 1, &out, &out_len) == 0);

    size_t f1 = suite_prov_frame_build(msg, sizeof(msg), 18, 1,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f1, 2, &out, &out_len) == 0);
    // 中间包重复:幂等忽略,长度不变。
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f1, 3, &out, &out_len) == 0);
    assert(r.len == 36);

    size_t f2 = suite_prov_frame_build(msg, sizeof(msg), 18, 2,
                                       s_frame_buf, sizeof(s_frame_buf));
    int ret = suite_prov_reasm_feed(&r, s_frame_buf, f2, 4, &out, &out_len);
    assert(ret == (int)sizeof(msg));
    assert(memcmp(out, msg, sizeof(msg)) == 0);
}

static void test_out_of_order_and_mid_without_start(void)
{
    uint8_t msg[40];
    memset(msg, 'Y', sizeof(msg));

    // 各帧保存在独立缓冲,避免切帧复用同一缓冲导致喂错数据。
    uint8_t fr0[SUITE_PROV_PAYLOAD_MIN + SUITE_PROV_HEADER_LEN];
    uint8_t fr2[SUITE_PROV_PAYLOAD_MIN + SUITE_PROV_HEADER_LEN];
    size_t f0 = suite_prov_frame_build(msg, sizeof(msg), 18, 0,
                                       fr0, sizeof(fr0));
    size_t f2 = suite_prov_frame_build(msg, sizeof(msg), 18, 2,
                                       fr2, sizeof(fr2));

    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, s_msg_buf, sizeof(s_msg_buf));
    const uint8_t *out = NULL;
    size_t out_len = 0;

    // 直接给末包(无 START)→ 拒绝。
    assert(suite_prov_reasm_feed(&r, fr2, f2, 0, &out, &out_len) == -1);

    assert(suite_prov_reasm_feed(&r, fr0, f0, 0, &out, &out_len) == 0);
    // 跳过 seq=1 直接给 seq=2 → 乱序,会话丢弃。
    assert(suite_prov_reasm_feed(&r, fr2, f2, 1, &out, &out_len) == -1);
    assert(!r.active && r.len == 0);

    // 保留位非法的 flags。
    uint8_t bad[] = {0x04, 0x00, 'a'};
    assert(suite_prov_reasm_feed(&r, bad, sizeof(bad), 2, &out, &out_len) == -1);
    // 过短帧。
    uint8_t tiny[] = {0x03};
    assert(suite_prov_reasm_feed(&r, tiny, 1, 2, &out, &out_len) == -1);
}

static void test_half_message_timeout(void)
{
    uint8_t msg[40];
    memset(msg, 'Z', sizeof(msg));

    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, s_msg_buf, sizeof(s_msg_buf));
    const uint8_t *out = NULL;
    size_t out_len = 0;

    size_t f0 = suite_prov_frame_build(msg, sizeof(msg), 18, 0,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f0, 1000,
                                 &out, &out_len) == 0);
    assert(r.active && r.len == 18);
    assert(!suite_prov_reasm_tick(&r, 1000 + SUITE_PROV_REASM_TIMEOUT_MS));
    // 恰好 3000ms 不算超时;3001ms 清空。
    assert(suite_prov_reasm_tick(&r, 1000 + SUITE_PROV_REASM_TIMEOUT_MS + 1));
    assert(!r.active && r.len == 0);

    // 超时后旧会话的中间包到达应被拒绝;START 可正常开新会话。
    size_t f1 = suite_prov_frame_build(msg, sizeof(msg), 18, 1,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f1, 5000,
                                 &out, &out_len) == -1);
    assert(roundtrip(msg, sizeof(msg), 18, &r, 5000) == (int)sizeof(msg));
}

static void test_oversize_message_rejected(void)
{
    uint8_t msg[40];
    memset(msg, 'Q', sizeof(msg));

    // 重组缓冲只给 20 字节:首帧 18 可收,第二帧装不下 → -1。
    uint8_t small[20];
    suite_prov_reasm_t r;
    suite_prov_reasm_init(&r, small, sizeof(small));
    const uint8_t *out = NULL;
    size_t out_len = 0;

    size_t f0 = suite_prov_frame_build(msg, sizeof(msg), 18, 0,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f0, 0, &out, &out_len) == 0);
    size_t f1 = suite_prov_frame_build(msg, sizeof(msg), 18, 1,
                                       s_frame_buf, sizeof(s_frame_buf));
    assert(suite_prov_reasm_feed(&r, s_frame_buf, f1, 1, &out, &out_len) == -1);
    assert(r.len == 0);
}

static void test_frame_build_guardrails(void)
{
    uint8_t m[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(suite_prov_frame_count(0, 18) == 0);
    assert(suite_prov_frame_count(1, 0) == 0);
    assert(suite_prov_frame_count(18, 18) == 1);
    assert(suite_prov_frame_count(19, 18) == 2);
    // 下标越界 / 输出容量不足 / 空指针。
    assert(suite_prov_frame_build(m, sizeof(m), 18, 5,
                                  s_frame_buf, sizeof(s_frame_buf)) == 0);
    assert(suite_prov_frame_build(m, sizeof(m), 18, 0,
                                  s_frame_buf, 10) == 0);
    assert(suite_prov_frame_build(NULL, sizeof(m), 18, 0,
                                  s_frame_buf, sizeof(s_frame_buf)) == 0);
}

static void test_pin_verify(void)
{
    suite_prov_pin_t p;
    suite_prov_pin_start(&p, "123456");
    assert(!suite_prov_pin_authed(&p));

    // 长度不对也算一次失败。
    assert(suite_prov_pin_verify(&p, "12345", 5) == SUITE_PROV_PIN_BAD);
    assert(suite_prov_pin_verify(&p, "654321", 6) == SUITE_PROV_PIN_BAD);
    assert(p.fails == 2 && !suite_prov_pin_authed(&p));

    // 第三次仍错 → 锁定,设备应断开。
    assert(suite_prov_pin_verify(&p, "000000", 6) == SUITE_PROV_PIN_LOCKED);
    assert(suite_prov_pin_verify(&p, "123456", 6) == SUITE_PROV_PIN_LOCKED);

    // 新一轮配对:前两次错,第三次对 → 通过。
    suite_prov_pin_start(&p, "246810");
    assert(suite_prov_pin_verify(&p, "111111", 6) == SUITE_PROV_PIN_BAD);
    assert(suite_prov_pin_verify(&p, "222222", 6) == SUITE_PROV_PIN_BAD);
    assert(suite_prov_pin_verify(&p, "246810", 6) == SUITE_PROV_PIN_OK);
    assert(suite_prov_pin_authed(&p));
    // 通过后重复 hello 恒为 OK,不再计失败。
    assert(suite_prov_pin_verify(&p, "000000", 6) == SUITE_PROV_PIN_OK);

    suite_prov_pin_reset(&p);
    assert(!suite_prov_pin_authed(&p) && p.fails == 0);
}

int main(void)
{
    test_single_packet();
    test_multi_packet_flags_and_seq();
    test_duplicate_seq_is_idempotent();
    test_out_of_order_and_mid_without_start();
    test_half_message_timeout();
    test_oversize_message_rejected();
    test_frame_build_guardrails();
    test_pin_verify();
    return 0;
}
