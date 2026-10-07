// main/suite_storage.c —— NVS 配置持久化实现。
#include "suite_storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "suite_storage";
static const char *NS = "suite";

// Kconfig 默认凭据仅用于开发引导:仓库里保持为空,不在源码提交秘密。
// 留空且 NVS 无凭据时保持未配置状态,等生产配网阶段接入。

esp_err_t suite_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 只在 NVS 布局失效时才擦除:这会丢历史配置,属已知代价并打日志。
        ESP_LOGW(TAG, "NVS 需要重格式化,已存配置将丢失");
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t suite_storage_wifi_get(suite_wifi_cred_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(out->ssid);
    err = nvs_get_str(h, "w_ssid", out->ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(out->password);
        err = nvs_get_str(h, "w_pass", out->password, &len);
    }
    nvs_close(h);
    if (err != ESP_OK) return err == ESP_ERR_NVS_INVALID_LENGTH ? ESP_ERR_NVS_INVALID_LENGTH : err;
    if (out->ssid[0] == '\0') return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t suite_storage_wifi_set(const suite_wifi_cred_t *in)
{
    if (!in || in->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "w_ssid", in->ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "w_pass", in->password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool suite_storage_wifi_exists(void)
{
    suite_wifi_cred_t c;
    return suite_storage_wifi_get(&c) == ESP_OK;
}

esp_err_t suite_storage_city_get(suite_city_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    // 默认:北京,UTC+8。无存储值时直接用默认,不视为错误。
    // 城市名必须是中文(与 k_cities 一致):子集字库无小写拉丁字形,
    // "Beijing" 会上屏成 "B"+空白(设备实测)。
    *out = (suite_city_t){
        .name = "北京", .lat_x1e4 = 395427, .lon_x1e4 = 1163916, .tz_off_s = 8 * 3600,
    };
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;  // 无记录 → 默认值
    int32_t v = 0;
    err = nvs_get_i32(h, "city_lat", &v);
    if (err == ESP_OK) out->lat_x1e4 = v;
    err = nvs_get_i32(h, "city_lon", &v);
    if (err == ESP_OK) out->lon_x1e4 = v;
    err = nvs_get_i32(h, "city_tz", &v);
    if (err == ESP_OK) out->tz_off_s = v;
    size_t len = sizeof(out->name);
    err = nvs_get_str(h, "city_name", out->name, &len);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        // 截断的名称也照常显示(已按 sizeof 写满),不视为错误。
        err = ESP_OK;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t suite_storage_city_set(const suite_city_t *in)
{
    if (!in || in->lat_x1e4 < -900000 || in->lat_x1e4 > 900000 ||
        in->lon_x1e4 < -1800000 || in->lon_x1e4 > 1800000) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "city_name", in->name);
    if (err == ESP_OK) err = nvs_set_i32(h, "city_lat", in->lat_x1e4);
    if (err == ESP_OK) err = nvs_set_i32(h, "city_lon", in->lon_x1e4);
    if (err == ESP_OK) err = nvs_set_i32(h, "city_tz", in->tz_off_s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
