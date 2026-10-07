// main/suite_weather_logic.c —— 天气纯逻辑实现。
#include "suite_weather_logic.h"

#include <stdio.h>
#include <stdlib.h>

bool suite_wx_build_url(char *url, size_t cap, double lat, double lon)
{
    if (!url || cap == 0) return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    int n = snprintf(url, cap,
                     "https://api.open-meteo.com/v1/forecast"
                     "?latitude=%.4f&longitude=%.4f"
                     "&current=temperature_2m,weather_code"
                     "&timezone=auto",
                     lat, lon);
    return n > 0 && (size_t)n < cap;
}

suite_wx_group_t suite_wx_code_group(int wmo_code)
{
    if (wmo_code == 0) return SUITE_WX_CLEAR;
    if (wmo_code == 1 || wmo_code == 2) return SUITE_WX_PARTLY;
    if (wmo_code == 3) return SUITE_WX_CLOUDY;
    if (wmo_code == 45 || wmo_code == 48) return SUITE_WX_FOG;
    if (wmo_code >= 51 && wmo_code <= 57) return SUITE_WX_DRIZZLE;
    if (wmo_code >= 61 && wmo_code <= 67) return SUITE_WX_RAIN;
    if (wmo_code >= 71 && wmo_code <= 77) return SUITE_WX_SNOW;
    if (wmo_code >= 80 && wmo_code <= 82) return SUITE_WX_RAIN;
    if (wmo_code >= 85 && wmo_code <= 86) return SUITE_WX_SNOW;
    if (wmo_code >= 95 && wmo_code <= 99) return SUITE_WX_STORM;
    return SUITE_WX_UNKNOWN;
}

// 跳过一个 JSON 字符串字面量。i 指向开头引号,返回结尾引号下标;越界返回 -1。
static long skip_string(const char *json, long i, long len)
{
    i++;  // 过开头引号
    while (i < len) {
        if (json[i] == '\\') {
            i += 2;
            continue;
        }
        if (json[i] == '"') return i;
        i++;
    }
    return -1;
}

// 定位 "key": 之后的位置。只匹配带引号的键,字符串值内部出现的同名文本不算。
static long find_key_value_pos(const char *json, const char *key)
{
    long len = 0;
    while (json[len] != '\0') {
        len++;
        if (len > 16384) return -1;  // 有界扫描上限
    }

    long klen = 0;
    while (key[klen] != '\0') klen++;

    long i = 0;
    while (i < len) {
        if (json[i] == '"') {
            long end = skip_string(json, i, len);
            if (end < 0) return -1;
            // 该字符串恰好是目标键:后跟可选空白 + ':'。
            if (end - i - 1 == klen) {
                long j = i + 1;
                for (long k = 0; k < klen; k++) {
                    if (json[j + k] != key[k]) goto next;
                }
                j = end + 1;
                while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                                   json[j] == '\n' || json[j] == '\r')) {
                    j++;
                }
                if (j < len && json[j] == ':') {
                    j++;
                    while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                                       json[j] == '\n' || json[j] == '\r')) {
                        j++;
                    }
                    return j;
                }
            }
        next:
            i = end + 1;
            continue;
        }
        i++;
    }
    return -1;
}

static bool parse_number(const char *json, long pos, double *out_d, long *out_i)
{
    char buf[32];
    long n = 0;
    while (n < (long)sizeof(buf) - 1) {
        char c = json[pos + n];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E') {
            buf[n] = c;
            n++;
            continue;
        }
        break;
    }
    if (n == 0) return false;
    buf[n] = '\0';

    char *endp = NULL;
    double v = strtod(buf, &endp);  // strtod 在纯逻辑测试里由宿主 libc 提供
    if (endp == buf) return false;
    if (out_d) *out_d = v;
    if (out_i) *out_i = (long)v;
    return true;
}

bool suite_wx_extract_double(const char *json, const char *key, double *out)
{
    long pos = find_key_value_pos(json, key);
    if (pos < 0) return false;
    return parse_number(json, pos, out, NULL);
}

bool suite_wx_extract_int(const char *json, const char *key, long *out)
{
    long pos = find_key_value_pos(json, key);
    if (pos < 0) return false;
    return parse_number(json, pos, NULL, out);
}

bool suite_wx_is_fresh(int64_t fetched_epoch, int64_t now_epoch)
{
    if (fetched_epoch <= 0) return false;
    return (now_epoch - fetched_epoch) < SUITE_WX_FRESH_WINDOW_S;
}
