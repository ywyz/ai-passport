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

// 新鲜度:2 小时内的数据视为可用,超时为"过期"(独立提示,不静默丢弃)。
#define SUITE_WX_FRESH_WINDOW_S ((int64_t)2 * 3600)
#define SUITE_WX_REFRESH_PERIOD_S ((int64_t)30 * 60)
bool suite_wx_is_fresh(int64_t fetched_epoch, int64_t now_epoch);
