// main/suite_weather.h —— Open-Meteo 天气抓取(设备侧)。
// 仅在连接在线且时间可信后发起 HTTPS;失败/离线保留旧数据并标记过期。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;             // 至少成功抓取过一次
    bool stale;             // 数据超过新鲜窗口
    double temperature;     // 摄氏度
    long wmo_code;          // WMO 天气码(原始值,UI 归组)
    int64_t fetched_epoch;  // 抓取时刻
} suite_weather_data_t;

esp_err_t suite_weather_start(void);
void suite_weather_stop(void);

// 拷贝当前天气快照(内部加锁)。从未抓到数据时 valid=false。
bool suite_weather_get(suite_weather_data_t *out);

// 请求立即刷新(状态页 OK 键)。离线/时间不可信时拒绝并返回 false。
bool suite_weather_request_refresh(void);
