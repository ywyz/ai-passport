// main/suite_weather_logic.h —— 天气客户端纯逻辑(可主机测试)。
// Open-Meteo 免费接口(Q-08 用户已确认):无 key,需署名;请求/解析全部
// 有界,不整文件进 RAM。真实 HTTP 在设备侧 suite_weather.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WMO weather code 归组,UI 层据此取文案/图标。
typedef enum {
    SUITE_WX_CLEAR = 0,
    SUITE_WX_PARTLY,
    SUITE_WX_CLOUDY,
    SUITE_WX_FOG,
    SUITE_WX_DRIZZLE,
    SUITE_WX_RAIN,
    SUITE_WX_SNOW,
    SUITE_WX_STORM,
    SUITE_WX_UNKNOWN,
} suite_wx_group_t;

// 生成 Open-Meteo current-weather 请求 URL。
// 成功返回 true 并写入 url(含 NUL);缓冲不足返回 false。
bool suite_wx_build_url(char *url, size_t cap, double lat, double lon);

suite_wx_group_t suite_wx_code_group(int wmo_code);

// 有界 JSON 数字字段提取:在 json 里找 "key" 后的数值(跳过字符串值,
// 支持负号与小数)。找到返回 true。json 必须以 NUL 结尾,长度有调用方限制。
bool suite_wx_extract_double(const char *json, const char *key, double *out);
bool suite_wx_extract_int(const char *json, const char *key, long *out);

// 响应体收集:按整次响应的累计长度判定上限,超限即整次作废——即使开头
// 已经出现必需字段,也不从截断的 JSON 里"解析成功"。cap 含末尾 NUL 预留,
// 因此单次响应最多接受 cap-1 字节数据。
#define SUITE_WX_BODY_MAX 8192
typedef struct {
    char  *buf;       // 调用方缓冲,cap 字节
    size_t cap;       // 总容量,含终止符预留
    size_t len;       // 已收集字节数
    bool   overflow;  // 本次响应已超限(内容不可再解析)
} suite_wx_body_t;

// 开始新一次响应收集:清空长度与超限标记(缓冲绑定由调用方在构造时给出)。
void suite_wx_body_reset(suite_wx_body_t *body);

// 追加一个响应分块。累计超过 cap-1 时置超限并丢弃本次全部内容;
// 超限后到的分块不再计入。data/len 为 0 时忽略。
void suite_wx_body_append(suite_wx_body_t *body, const void *data, size_t len);

bool suite_wx_body_overflow(const suite_wx_body_t *body);

// 返回 NUL 结尾的响应文本(可能为空串);超限或未绑定时返回 NULL。
const char *suite_wx_body_text(const suite_wx_body_t *body);

// 新鲜度:2 小时内的数据视为可用,超时为"过期"(独立提示,不静默丢弃)。
#define SUITE_WX_FRESH_WINDOW_S ((int64_t)2 * 3600)
// 刷新节奏:成功后 30 min 周期;失败后 60 s 重试;离线/未校时 2 s 复查。
#define SUITE_WX_REFRESH_PERIOD_S ((int64_t)30 * 60)
#define SUITE_WX_RETRY_PERIOD_S ((int64_t)60)
#define SUITE_WX_PROBE_PERIOD_S ((int64_t)2)
bool suite_wx_is_fresh(int64_t fetched_epoch, int64_t now_epoch);
