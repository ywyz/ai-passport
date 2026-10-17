// main/suite_prov_proto.c —— 配网分包帧协议与配对码校验实现(纯逻辑)。
#include "suite_prov_proto.h"

#include <string.h>

// ---- 发送侧切帧 ----

size_t suite_prov_frame_count(size_t msg_len, size_t mtu_payload)
{
    if (mtu_payload == 0) return 0;
    return (msg_len + mtu_payload - 1) / mtu_payload;
}

size_t suite_prov_frame_build(const uint8_t *msg, size_t msg_len,
                              size_t mtu_payload, size_t frame_idx,
                              uint8_t *out, size_t out_cap)
{
    if (msg == NULL || out == NULL || mtu_payload == 0) return 0;
    size_t frames = suite_prov_frame_count(msg_len, mtu_payload);
    if (frames == 0 || frame_idx >= frames) return 0;
    if (out_cap < mtu_payload + SUITE_PROV_HEADER_LEN) return 0;

    size_t offset = frame_idx * mtu_payload;
    size_t chunk = msg_len - offset;
    if (chunk > mtu_payload) chunk = mtu_payload;

    uint8_t flags;
    if (frames == 1) {
        flags = SUITE_PROV_FLAG_SINGLE;
    } else if (frame_idx == 0) {
        flags = SUITE_PROV_FLAG_START;
    } else if (frame_idx == frames - 1) {
        flags = SUITE_PROV_FLAG_END;
    } else {
        flags = 0;
    }

    out[0] = flags;
    out[1] = (uint8_t)frame_idx;
    memcpy(out + SUITE_PROV_HEADER_LEN, msg + offset, chunk);
    return chunk + SUITE_PROV_HEADER_LEN;
}

// ---- 接收侧重组 ----

void suite_prov_reasm_init(suite_prov_reasm_t *r, uint8_t *buf, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
}

static void reasm_reset(suite_prov_reasm_t *r)
{
    r->len = 0;
    r->have_seq = false;
    r->active = false;
}

int suite_prov_reasm_feed(suite_prov_reasm_t *r, const uint8_t *pkt,
                          size_t pkt_len, uint32_t now_ms,
                          const uint8_t **out, size_t *out_len)
{
    if (r == NULL || pkt == NULL || out == NULL || out_len == NULL) return -1;
    if (pkt_len < SUITE_PROV_HEADER_LEN) return -1;

    uint8_t flags = pkt[0];
    uint8_t seq = pkt[1];
    const uint8_t *payload = pkt + SUITE_PROV_HEADER_LEN;
    size_t payload_len = pkt_len - SUITE_PROV_HEADER_LEN;

    bool is_start = (flags & SUITE_PROV_FLAG_START) != 0;
    bool is_end = (flags & SUITE_PROV_FLAG_END) != 0;
    // flags 只允许 0x00..0x03,其它位为保留位。
    if (flags & ~(uint8_t)SUITE_PROV_FLAG_SINGLE) return -1;

    if (is_start) {
        // START(含单包)总是开新会话,实现"按 START 重置重组会话"。
        r->active = !is_end;
        r->len = 0;
        r->seq = seq;
        r->have_seq = true;
        r->start_ms = now_ms;
    } else {
        // 没有进行中的会话却收到中间/末包:丢弃。
        if (!r->active || !r->have_seq) return -1;
        if (seq == r->seq) {
            // 重复包:幂等忽略(同一序号不二次追加)。
            return 0;
        }
        if (seq != (uint8_t)(r->seq + 1)) {
            // 乱序/丢包后到达:本会话无法续上,丢弃。
            reasm_reset(r);
            return -1;
        }
        r->seq = seq;
    }

    if (payload_len > r->cap - r->len) {
        // 超出消息缓冲:丢弃半条消息。
        reasm_reset(r);
        return -1;
    }
    memcpy(r->buf + r->len, payload, payload_len);
    r->len += payload_len;

    if (is_end) {
        size_t total = r->len;
        *out = r->buf;
        *out_len = total;
        reasm_reset(r);
        return (int)total;
    }
    return 0;
}

bool suite_prov_reasm_tick(suite_prov_reasm_t *r, uint32_t now_ms)
{
    if (!r->active) return false;
    // 用有符号差值比较,容忍毫秒计数器 32 位回绕。
    int32_t elapsed = (int32_t)(now_ms - r->start_ms);
    if (elapsed > SUITE_PROV_REASM_TIMEOUT_MS) {
        reasm_reset(r);
        return true;
    }
    return false;
}

// ---- 配对码校验 ----

void suite_prov_pin_start(suite_prov_pin_t *p,
                          const char expected[SUITE_PROV_PIN_LEN])
{
    memset(p, 0, sizeof(*p));
    memcpy(p->expected, expected, SUITE_PROV_PIN_LEN);
    p->expected[SUITE_PROV_PIN_LEN] = '\0';
}

void suite_prov_pin_reset(suite_prov_pin_t *p)
{
    memset(p, 0, sizeof(*p));
}

suite_prov_pin_result_t suite_prov_pin_verify(suite_prov_pin_t *p,
                                              const char *attempt, size_t len)
{
    if (p == NULL || attempt == NULL) return SUITE_PROV_PIN_BAD;
    if (p->authed) return SUITE_PROV_PIN_OK;
    if (p->fails >= SUITE_PROV_PIN_MAX_FAILS) return SUITE_PROV_PIN_LOCKED;
    if (len != SUITE_PROV_PIN_LEN ||
        memcmp(attempt, p->expected, SUITE_PROV_PIN_LEN) != 0) {
        p->fails += 1;
        return p->fails >= SUITE_PROV_PIN_MAX_FAILS
                   ? SUITE_PROV_PIN_LOCKED
                   : SUITE_PROV_PIN_BAD;
    }
    p->authed = true;
    p->fails = 0;
    return SUITE_PROV_PIN_OK;
}

bool suite_prov_pin_authed(const suite_prov_pin_t *p)
{
    return p != NULL && p->authed;
}

// ---- 连接会话状态 ----

void suite_prov_session_init(suite_prov_session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->conn = SUITE_PROV_CONN_NONE;
    s->mtu = SUITE_PROV_MTU_DEFAULT;
}

void suite_prov_session_connect(suite_prov_session_t *s, uint16_t conn,
                                const char expected_pin[SUITE_PROV_PIN_LEN])
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));  // 上一连接的授权/订阅/MTU 全部作废
    s->conn = conn;
    s->mtu = SUITE_PROV_MTU_DEFAULT;
    suite_prov_pin_start(&s->pin, expected_pin);
}

void suite_prov_session_disconnect(suite_prov_session_t *s, uint16_t conn)
{
    if (s == NULL || s->conn != conn) return;
    s->conn = SUITE_PROV_CONN_NONE;
    s->subscribed = false;
    s->mtu = SUITE_PROV_MTU_DEFAULT;
    suite_prov_pin_reset(&s->pin);
}

void suite_prov_session_subscribe(suite_prov_session_t *s, bool on)
{
    if (s == NULL) return;
    s->subscribed = on;
}

bool suite_prov_session_set_mtu(suite_prov_session_t *s, uint16_t conn,
                                uint16_t mtu)
{
    if (s == NULL || s->conn != conn || mtu < SUITE_PROV_MTU_DEFAULT) {
        return false;
    }
    s->mtu = mtu;
    return true;
}

bool suite_prov_session_status_allowed(const suite_prov_session_t *s)
{
    return s != NULL && s->conn != SUITE_PROV_CONN_NONE && s->subscribed &&
           suite_prov_pin_authed(&s->pin);
}

bool suite_prov_session_authorized(const suite_prov_session_t *s)
{
    return s != NULL && suite_prov_pin_authed(&s->pin);
}
