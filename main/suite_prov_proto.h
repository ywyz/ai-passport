// main/suite_prov_proto.h —— 手机 BLE 配网的分包帧协议与配对码校验(纯逻辑)。
// 不依赖 ESP-IDF/LVGL/FreeRTOS,时间由调用方注入,可直接主机测试。
//
// 帧格式(设备与网页严格一致):
//   每包 = [flags:1][seq:1][payload...]
//     flags bit0 = END,bit1 = START
//     单包 0x03;首包 0x02;中间包 0x00;末包 0x01
//   seq 逐包递增(单包也占一个序号),接收端据此丢弃重复/乱序包。
// 重组结果是一条完整的 UTF-8 JSON 消息。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUITE_PROV_HEADER_LEN 2
#define SUITE_PROV_FLAG_END 0x01
#define SUITE_PROV_FLAG_START 0x02
#define SUITE_PROV_FLAG_SINGLE (SUITE_PROV_FLAG_START | SUITE_PROV_FLAG_END)

// 重组后的单条 JSON 消息上限。provinces/cities 最大响应 < 800B,留余量。
#define SUITE_PROV_MSG_MAX 1100
// 单包净荷上限:协商 MTU 247 时 247 - ATT 头 3 - 帧头 2 = 242。
#define SUITE_PROV_PAYLOAD_MAX 242
// 默认 ATT_MTU 23 时的单包净荷:23 - 3 - 2 = 18。
#define SUITE_PROV_PAYLOAD_MIN 18
// 半条消息的重组超时:距首包超过该值未收齐即丢弃。
#define SUITE_PROV_REASM_TIMEOUT_MS 3000

// ---- 发送侧:消息切帧(纯函数) ----

// 给定单包净荷容量 mtu_payload(须 >0),返回 msg_len 字节消息需要的帧数。
size_t suite_prov_frame_count(size_t msg_len, size_t mtu_payload);

// 构造第 frame_idx 帧(下标从 0 起),返回写入 out 的总字节数(含帧头);
// 参数非法返回 0。out_cap 须不小于 mtu_payload + SUITE_PROV_HEADER_LEN。
size_t suite_prov_frame_build(const uint8_t *msg, size_t msg_len,
                              size_t mtu_payload, size_t frame_idx,
                              uint8_t *out, size_t out_cap);

// ---- 接收侧:帧重组 ----

typedef struct {
    uint8_t *buf;       // 调用方提供的重组缓冲(建议 SUITE_PROV_MSG_MAX)
    size_t cap;
    size_t len;         // 当前已累积净荷
    uint8_t seq;        // 上一包序号
    bool have_seq;      // 本会话是否已收到过包
    bool active;        // 正在重组一条多帧消息
    uint32_t start_ms;  // 首包时刻(调用方时钟)
} suite_prov_reasm_t;

void suite_prov_reasm_init(suite_prov_reasm_t *r, uint8_t *buf, size_t cap);

// 喂入一帧(含 flags/seq)。返回:
//   >0  一条完整消息的长度;*out 指向内部缓冲(下次 feed/init 前有效)
//    0  消息尚未收齐,继续等待
//   -1  帧非法(乱序/无 START/超长等),已丢弃当前会话
int suite_prov_reasm_feed(suite_prov_reasm_t *r, const uint8_t *pkt,
                          size_t pkt_len, uint32_t now_ms,
                          const uint8_t **out, size_t *out_len);

// 超时维护:距首包超过 SUITE_PROV_REASM_TIMEOUT_MS 的半条消息丢弃。
// 返回 true 表示确实清空了一个未完成会话。
bool suite_prov_reasm_tick(suite_prov_reasm_t *r, uint32_t now_ms);

// ---- 应用层 6 位配对码校验(纯逻辑) ----
//
// 安全说明:本机制仅为基础防护(防止附近的人随手连入),不是完整的授权
// 设计;链路层使用 Just Works 配对,不提供防窃听/中间人防护。

#define SUITE_PROV_PIN_LEN 6
#define SUITE_PROV_PIN_MAX_FAILS 3

typedef enum {
    SUITE_PROV_PIN_WAIT = 0,  // 尚未验证通过
    SUITE_PROV_PIN_OK,        // 本次验证通过
    SUITE_PROV_PIN_BAD,       // 本次错误但未达上限,可重试
    SUITE_PROV_PIN_LOCKED,    // 连续错误达上限,设备应主动断开
} suite_prov_pin_result_t;

typedef struct {
    char expected[SUITE_PROV_PIN_LEN + 1];
    uint8_t fails;
    bool authed;
} suite_prov_pin_t;

// 开始一轮配对会话:登记屏幕上展示的 6 位配对码,清零计数与授权状态。
void suite_prov_pin_start(suite_prov_pin_t *p,
                          const char expected[SUITE_PROV_PIN_LEN]);
void suite_prov_pin_reset(suite_prov_pin_t *p);

// 校验一次尝试(长度须恰为 6 位数字)。
suite_prov_pin_result_t suite_prov_pin_verify(suite_prov_pin_t *p,
                                              const char *attempt, size_t len);
bool suite_prov_pin_authed(const suite_prov_pin_t *p);

// ---- 连接会话状态(纯逻辑) ----
//
// 每个 BLE 连接一个独立会话:订阅状态、协商 MTU 与配对码授权都必须随
// 连接重建而重置 —— 上一次连接的授权与 MTU 绝不能被下一次连接继承。

#define SUITE_PROV_CONN_NONE 0xFFFFu
#define SUITE_PROV_MTU_DEFAULT 23u

typedef struct {
    uint16_t conn;         // 当前连接句柄;NONE 表示无连接
    bool subscribed;       // 当前连接已订阅 TX 通知
    uint16_t mtu;          // 当前连接协商的 ATT MTU(未协商为 DEFAULT)
    suite_prov_pin_t pin;  // 当前连接的配对码授权状态
} suite_prov_session_t;

void suite_prov_session_init(suite_prov_session_t *s);

// 新连接建立:登记句柄、清零订阅、恢复默认 MTU,并开始新一轮配对码
// 会话(上一次连接的授权不继承)。
void suite_prov_session_connect(suite_prov_session_t *s, uint16_t conn,
                                const char expected_pin[SUITE_PROV_PIN_LEN]);

// 断开:仅当句柄是当前连接时清理并恢复默认 MTU;旧连接的迟到断开事件
// 不得影响正在使用的新连接。
void suite_prov_session_disconnect(suite_prov_session_t *s, uint16_t conn);

void suite_prov_session_subscribe(suite_prov_session_t *s, bool on);

// 应用当前连接的 MTU 协商结果;句柄不是当前连接(旧连接迟到事件)或
// 数值小于默认 ATT MTU 时拒绝。返回是否已应用。
bool suite_prov_session_set_mtu(suite_prov_session_t *s, uint16_t conn,
                                uint16_t mtu);

// 主动状态通知的授权条件:已连接 + 已订阅 + 已通过配对码。
bool suite_prov_session_status_allowed(const suite_prov_session_t *s);

// 除 hello 外的命令都要求当前连接先通过配对码。
bool suite_prov_session_authorized(const suite_prov_session_t *s);
