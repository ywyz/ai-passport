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

    // 新鲜度:窗口内新鲜,超窗/无效时间过期。
    assert(suite_wx_is_fresh(1000, 1000 + SUITE_WX_FRESH_WINDOW_S - 1));
    assert(!suite_wx_is_fresh(1000, 1000 + SUITE_WX_FRESH_WINDOW_S));
    assert(!suite_wx_is_fresh(0, 100000));

    return 0;
}
