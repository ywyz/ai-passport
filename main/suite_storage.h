// main/suite_storage.h —— 运行配置持久化(NVS)。
// 保存 Wi-Fi 凭据与城市设置;写失败不覆盖旧值,读失败给默认值。
// 生产配网走浏览器 BLE(SC-02,另行阶段);本模块先承载 NVS 读写。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char ssid[33];
    char password[65];
} suite_wifi_cred_t;

typedef struct {
    char name[32];      // 展示名(英数,中文字体接入前不用中文)
    int32_t lat_x1e4;   // 纬度 ×1e4
    int32_t lon_x1e4;   // 经度 ×1e4
    int32_t tz_off_s;   // UTC 偏移秒(如 CST=28800)
} suite_city_t;

esp_err_t suite_storage_init(void);

esp_err_t suite_storage_wifi_get(suite_wifi_cred_t *out);   // 无凭据返回 ESP_ERR_NOT_FOUND
esp_err_t suite_storage_wifi_set(const suite_wifi_cred_t *in);
bool suite_storage_wifi_exists(void);

esp_err_t suite_storage_city_get(suite_city_t *out);        // 无城市返回默认(北京)
esp_err_t suite_storage_city_set(const suite_city_t *in);
