// main/suite_weather.c —— 天气抓取实现。
// 响应有界(8 KB 截断即弃),不整文件进 RAM;TLS 用系统证书包校验,
// 抓取前要求 SNTP 已校时,不以关闭证书验证换启动速度(SC-03)。
#include "suite_weather.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_storage.h"
#include "suite_weather_logic.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "suite_wx";

#define WX_TASK_STACK 6144
#define WX_BODY_MAX 8192
#define WX_HTTP_TIMEOUT_MS 10000

typedef struct {
    suite_weather_data_t data;
    SemaphoreHandle_t mtx;
    TaskHandle_t task;
    volatile bool run;
    volatile bool refresh_now;
} wx_ctx_t;

static wx_ctx_t s_wx;

static void wx_store(const suite_weather_data_t *d)
{
    xSemaphoreTake(s_wx.mtx, portMAX_DELAY);
    s_wx.data = *d;
    xSemaphoreGive(s_wx.mtx);
}

// 响应体收集缓冲:perform() 会在内部读完响应体并经事件回调投递,
// 之后再 esp_http_client_read() 读不到数据,必须在回调里收集。
static char s_body[WX_BODY_MAX];
static int s_body_len;

// esp_http_client 事件:收集 ON_DATA 分块,超出上限即截断丢弃。
static esp_err_t wx_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        int room = WX_BODY_MAX - 1 - s_body_len;
        int copy = evt->data_len < room ? evt->data_len : room;
        if (copy > 0) {
            memcpy(s_body + s_body_len, evt->data, copy);
            s_body_len += copy;
        }
    }
    return ESP_OK;
}

// 抓取一次;成功返回 true 并填充 out。
static bool wx_fetch_once(const suite_city_t *city, suite_weather_data_t *out)
{
    char url[160];
    if (!suite_wx_build_url(url, sizeof(url),
                            city->lat_x1e4 / 10000.0, city->lon_x1e4 / 10000.0)) {
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = wx_http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WX_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    bool ok = false;
    s_body_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        int64_t now = suite_connection_now();
        // 回调已收集响应体;空体(被截断/无数据)按解析失败处理。
        if (s_body_len <= 0) {
            ESP_LOGW(TAG, "响应体为空");
        } else {
            s_body[s_body_len] = '\0';
            double temp = 0.0;
            long code = 0;
            if (suite_wx_extract_double(s_body, "temperature_2m", &temp) &&
                suite_wx_extract_int(s_body, "weather_code", &code)) {
                memset(out, 0, sizeof(*out));
                out->valid = true;
                out->temperature = temp;
                out->wmo_code = code;
                out->fetched_epoch = now;
                out->stale = !suite_wx_is_fresh(out->fetched_epoch,
                                                suite_connection_now());
                ok = true;
            } else {
                ESP_LOGW(TAG, "响应缺少必需字段");
            }
        }
    } else {
        ESP_LOGW(TAG, "天气请求失败: %s HTTP %d",
                 esp_err_to_name(err), esp_http_client_get_status_code(client));
    }

    esp_http_client_cleanup(client);
    return ok;
}

static void wx_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 未就绪(离线/未校时)2s 复查;成功 30min;失败 60s 重试。
        // 首轮即尝试:在线且时间可信时开机数秒内出天气。
        int64_t wait_s;
        if (suite_connection_state() == SUITE_CONN_ONLINE &&
            suite_time_is_trusted(suite_connection_time(),
                                  suite_connection_now())) {
            suite_city_t city;
            (void)suite_storage_city_get(&city);

            suite_weather_data_t d;
            if (wx_fetch_once(&city, &d)) {
                wx_store(&d);
                ESP_LOGI(TAG, "天气更新: %.1f C code=%ld", d.temperature,
                         d.wmo_code);
                wait_s = SUITE_WX_REFRESH_PERIOD_S;
            } else {
                // 失败保留旧数据,新鲜度随时间自然过期(UI 独立提示)。
                xSemaphoreTake(s_wx.mtx, portMAX_DELAY);
                if (s_wx.data.valid) {
                    s_wx.data.stale = !suite_wx_is_fresh(
                        s_wx.data.fetched_epoch, suite_connection_now());
                }
                xSemaphoreGive(s_wx.mtx);
                wait_s = 60;
            }
        } else {
            wait_s = 2;
        }

        // 分片睡眠:及时响应退出与手动刷新。
        for (int64_t i = 0; s_wx.run && !s_wx.refresh_now && i < wait_s;
             i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!s_wx.run) break;
    }
    s_wx.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t suite_weather_start(void)
{
    if (s_wx.task) return ESP_OK;
    memset(&s_wx, 0, sizeof(s_wx));
    s_wx.mtx = xSemaphoreCreateMutex();
    if (!s_wx.mtx) return ESP_ERR_NO_MEM;
    s_wx.run = true;
    if (xTaskCreate(wx_task, "suite_wx", WX_TASK_STACK, NULL, 4, &s_wx.task) !=
        pdPASS) {
        vSemaphoreDelete(s_wx.mtx);
        s_wx.mtx = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void suite_weather_stop(void)
{
    s_wx.run = false;
    s_wx.refresh_now = true;  // 唤醒睡眠分片促退出
}

bool suite_weather_get(suite_weather_data_t *out)
{
    if (!out || !s_wx.mtx) return false;
    xSemaphoreTake(s_wx.mtx, portMAX_DELAY);
    *out = s_wx.data;
    if (out->valid) {
        out->stale = !suite_wx_is_fresh(out->fetched_epoch,
                                        suite_connection_now());
    }
    xSemaphoreGive(s_wx.mtx);
    return out->valid;
}

bool suite_weather_request_refresh(void)
{
    if (!s_wx.run) return false;
    if (suite_connection_state() != SUITE_CONN_ONLINE) return false;
    if (!suite_time_is_trusted(suite_connection_time(),
                               suite_connection_now())) return false;
    s_wx.refresh_now = true;
    return true;
}
