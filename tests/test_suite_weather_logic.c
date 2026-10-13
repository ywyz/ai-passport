// tests/test_suite_weather_logic.c —— 天气纯逻辑主机测试。
#include <assert.h>
#include <string.h>

#include "suite_weather_logic.h"

int main(void) {
    char url[160];

    // URL 构造:合法坐标成功;越界拒绝。
    assert(suite_wx_build_url(url, sizeof(url), 39.5427, 116.3916));
    assert(strstr(url, "https://api.open-meteo.com/v1/forecast") == url);
    assert(strstr(url, "latitude=39.5427") && strstr(url, "longitude=116.3916"));
    assert(strstr(url, "current=temperature_2m,weather_code"));
    assert(strstr(url, "timezone=auto"));
    assert(!suite_wx_build_url(url, sizeof(url), 91.0, 0.0));
    assert(!suite_wx_build_url(url, sizeof(url), 0.0, 180.5));

    // 缓冲不足拒绝。
    char tiny[16];
    assert(!suite_wx_build_url(tiny, sizeof(tiny), 39.5, 116.4));

    // WMO 码归组。
    assert(suite_wx_code_group(0) == SUITE_WX_CLEAR);
    assert(suite_wx_code_group(2) == SUITE_WX_PARTLY);
    assert(suite_wx_code_group(3) == SUITE_WX_CLOUDY);
    assert(suite_wx_code_group(45) == SUITE_WX_FOG);
    assert(suite_wx_code_group(53) == SUITE_WX_DRIZZLE);
    assert(suite_wx_code_group(63) == SUITE_WX_RAIN);
    assert(suite_wx_code_group(75) == SUITE_WX_SNOW);
    assert(suite_wx_code_group(95) == SUITE_WX_STORM);
    assert(suite_wx_code_group(42) == SUITE_WX_UNKNOWN);

    // 数字字段提取:字符串值内部的键名文本被整体跳过,不影响真实字段。
    const char *json =
        "{\"latitude\":39.5,\"current_weather\":false,"
        "\"note\":\"temperature_2m and weather_code mentioned in text\","
        "\"current\":{\"temperature_2m\":18.2,\"weather_code\":3,"
        "\"is_day\":1}}";
    double d = 0;
    long v = 0;
    assert(suite_wx_extract_double(json, "temperature_2m", &d));
    assert(d > 18.19 && d < 18.21);
    assert(suite_wx_extract_int(json, "weather_code", &v) && v == 3);
    assert(suite_wx_extract_int(json, "is_day", &v) && v == 1);
    assert(!suite_wx_extract_double(json, "missing", &d));

    // 负温度。
    const char *cold = "{\"current\":{\"temperature_2m\":-7.5}}";
    assert(suite_wx_extract_double(cold, "temperature_2m", &d));
    assert(d > -7.51 && d < -7.49);

    // 回归:Open-Meteo 真实响应里 current_units 的字符串值同名键在前,
    // 数值键在后;必须跳过字符串值继续向后找。
    const char *real =
        "{\"latitude\":39.89,\"current_units\":{"
        "\"time\":\"iso8601\",\"interval\":\"seconds\","
        "\"temperature_2m\":\"°C\",\"weather_code\":\"wmo code\"},"
        "\"current\":{\"time\":\"2026-10-07T20:00\",\"interval\":900,"
        "\"temperature_2m\":21.5,\"weather_code\":0}}";
    assert(suite_wx_extract_double(real, "temperature_2m", &d));
    assert(d > 21.49 && d < 21.51);
    assert(suite_wx_extract_int(real, "weather_code", &v) && v == 0);
    // 值全是字符串(无数值键)时仍判失败。
    assert(!suite_wx_extract_double("{\"u\":{\"temperature_2m\":\"°C\"}}",
                                    "temperature_2m", &d));

    // 新鲜度:窗口内新鲜,超窗/无效时间过期。
    assert(suite_wx_is_fresh(1000, 1000 + SUITE_WX_FRESH_WINDOW_S - 1));
    assert(!suite_wx_is_fresh(1000, 1000 + SUITE_WX_FRESH_WINDOW_S));
    assert(!suite_wx_is_fresh(0, 100000));

    // 刷新节奏与响应上限常量:行为由任务级测试
    // (tests/test_suite_weather_runtime.c)按绝对毫秒/字节断言,这里的
    // 常量值本身也是契约的一部分。
    assert(SUITE_WX_REFRESH_PERIOD_S == 30 * 60);
    assert(SUITE_WX_RETRY_PERIOD_S == 60);
    assert(SUITE_WX_PROBE_PERIOD_S == 2);
    assert(SUITE_WX_BODY_MAX == 8192);

    // 响应收集:cap-1 为最大合法响应体,超限即整次作废。
    char store[64];
    suite_wx_body_t body = { .buf = store, .cap = sizeof(store) };
    suite_wx_body_reset(&body);
    assert(!suite_wx_body_overflow(&body));
    assert(strcmp(suite_wx_body_text(&body), "") == 0);
    suite_wx_body_append(&body, "hello", 5);
    assert(strcmp(suite_wx_body_text(&body), "hello") == 0);
    assert(!suite_wx_body_overflow(&body));

    // 多个分块累计恰好到上限成功;再多 1 字节即超限,且不留截断内容。
    char filler[63];
    memset(filler, 'x', sizeof(filler));
    suite_wx_body_reset(&body);
    suite_wx_body_append(&body, filler, 40);
    suite_wx_body_append(&body, filler, 23);
    assert(!suite_wx_body_overflow(&body));
    assert(strlen(suite_wx_body_text(&body)) == sizeof(filler));
    suite_wx_body_append(&body, "y", 1);
    assert(suite_wx_body_overflow(&body));
    assert(suite_wx_body_text(&body) == NULL);

    // 单块一次超限;reset 清除上一次的超限标记(每次请求重新收集)。
    suite_wx_body_reset(&body);
    suite_wx_body_append(&body, filler, sizeof(filler) + 1);
    assert(suite_wx_body_overflow(&body));
    assert(suite_wx_body_text(&body) == NULL);
    suite_wx_body_reset(&body);
    assert(!suite_wx_body_overflow(&body));
    assert(strcmp(suite_wx_body_text(&body), "") == 0);

    // 必需字段位于开头、后续内容超限:整次失败,禁止从截断内容解析。
    suite_wx_body_reset(&body);
    const char *head = "{\"current\":{\"temperature_2m\":18.2,\"weather_code\":3";
    suite_wx_body_append(&body, head, strlen(head));
    assert(suite_wx_extract_double(suite_wx_body_text(&body),
                                   "temperature_2m", &d));
    assert(suite_wx_extract_int(suite_wx_body_text(&body),
                                "weather_code", &v) && v == 3);
    suite_wx_body_append(&body, filler, sizeof(store));
    assert(suite_wx_body_overflow(&body));
    assert(suite_wx_body_text(&body) == NULL);

    // 设备缓冲边界:8191 字节合法并保留终止符,8192 字节(超 1)拒绝。
    static char big_store[SUITE_WX_BODY_MAX];
    suite_wx_body_t big = { .buf = big_store, .cap = sizeof(big_store) };
    suite_wx_body_reset(&big);
    for (int i = 0; i < 130; i++) {  // 130 × 63 = 8190
        suite_wx_body_append(&big, filler, sizeof(filler));
    }
    assert(!suite_wx_body_overflow(&big));
    suite_wx_body_append(&big, "y", 1);
    assert(!suite_wx_body_overflow(&big));
    assert(strlen(suite_wx_body_text(&big)) == SUITE_WX_BODY_MAX - 1);
    assert(suite_wx_body_text(&big)[SUITE_WX_BODY_MAX - 1] == '\0');
    suite_wx_body_append(&big, "y", 1);
    assert(suite_wx_body_overflow(&big));
    assert(suite_wx_body_text(&big) == NULL);

    return 0;
}
