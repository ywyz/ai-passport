// main/suite_weather.c —— 天气抓取实现。
// 响应有界(8 KB 超限整次丢弃,不解析截断内容),不整文件进 RAM;
// TLS 用系统证书包校验,抓取前要求 SNTP 已校时,不以关闭证书验证换启动速度(SC-03)。
#include "suite_weather.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_storage.h"
#include "suite_weather_logic.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "suite_wx";

#define WX_TASK_STACK 6144
#define WX_HTTP_TIMEOUT_MS 10000

typedef struct {
    suite_weather_data_t data;
    SemaphoreHandle_t mtx;
    // 刷新请求:二值信号量同时充当"有待处理的请求"标记与等待唤醒源。
    // 抓取期间到达的请求会留在信号量里,由下一轮消费,不会因清零而丢失;
    // 等待期间重复请求(信号量已满)天然合并为一次。
    SemaphoreHandle_t wake;
    TaskHandle_t task;
    volatile bool run;
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
// 上限判定与整次作废规则在 suite_weather_logic.c(纯逻辑,主机可测)。
static char s_body[SUITE_WX_BODY_MAX];
static suite_wx_body_t s_resp = { .buf = s_body, .cap = SUITE_WX_BODY_MAX };

// esp_http_client 事件:收集 ON_DATA 分块。超限按整次响应累计长度判定,
// 由收集器标记;超限后收集器不提供文本,回调不再改写结果。
static esp_err_t wx_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        suite_wx_body_append(&s_resp, evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

// 抓取前置条件:在线且时间可信(SNTP 同步在重同步窗口内)。
static bool wx_ready(void)
{
    return suite_connection_state() == SUITE_CONN_ONLINE &&
           suite_time_is_trusted(suite_connection_time(),
                                 suite_connection_now());
}

// 抓取一次;成功返回 true 并填充 out。
static bool wx_fetch_once(const suite_city_t *city, suite_weather_data_t *out)
{
    char url[160];
    if (!suite_wx_build_url(url, sizeof(url),
                            city->lat_x1e4 / 10000.0, city->lon_x1e4 / 10000.0)) {
        ESP_LOGW(TAG, "城市坐标无效,未发起请求");
        return false;
    }

    // 每次请求重新初始化收集状态:上一次的超限标记不能泄漏到本次。
    suite_wx_body_reset(&s_resp);

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = wx_http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WX_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    // 诊断:TLS 会话建立(mbedtls_ssl_setup)需要内部堆上 ~12-14K 连续内存,
    // BLE 常驻后此时刻的余量决定天气功能可用性。DMA 池单独列出:硬件加速
    // 与 Wi-Fi 驱动只认 DMA 内存,总余量充足也可能因 DMA 池耗尽而失败。
    ESP_LOGI(TAG, "请求前堆: free=%lu 最大连续=%lu | DMA free=%lu 最大=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        const char *body = suite_wx_body_text(&s_resp);
        if (suite_wx_body_overflow(&s_resp)) {
            // 必需字段可能已经出现在开头,但截断的 JSON 不可靠:整次失败,
            // 否则会从残缺内容里提取字段并报告误导性的"成功"。
            ESP_LOGW(TAG, "响应超过 %d 字节上限,整次丢弃",
                     (int)(SUITE_WX_BODY_MAX - 1));
        } else if (!body || body[0] == '\0') {
            ESP_LOGW(TAG, "响应体为空");
        } else {
            int64_t now = suite_connection_now();
            double temp = 0.0;
            long code = 0;
            if (suite_wx_extract_double(body, "temperature_2m", &temp) &&
                suite_wx_extract_int(body, "weather_code", &code)) {
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
        if (!s_wx.run) break;

        int64_t wait_s;
        if (wx_ready()) {
            suite_city_t city;
            suite_weather_data_t d;
            if (suite_storage_city_get(&city) != ESP_OK) {
                // 读不到城市时不构造无效请求,等下一轮复查。
                ESP_LOGW(TAG, "城市配置读取失败");
                wait_s = SUITE_WX_PROBE_PERIOD_S;
            } else if (wx_fetch_once(&city, &d)) {
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
                wait_s = SUITE_WX_RETRY_PERIOD_S;
            }
        } else {
            // 未就绪(离线/未校时)2s 复查;该状态下刷新 API 拒绝请求,
            // 这里只睡满整段,不会形成无延时的忙循环。
            wait_s = SUITE_WX_PROBE_PERIOD_S;
        }

        if (!s_wx.run) break;
        // 单次带超时等待:等待期间到达的刷新请求(含抓取过程中到达、由
        // 信号量保留下来的请求)与停止都会提前唤醒,下一轮立即按节拍处理;
        // 其余情况睡满 wait_s(成功 30min、失败 60s、未就绪 2s)。
        (void)xSemaphoreTake(s_wx.wake,
                             pdMS_TO_TICKS((TickType_t)(wait_s * 1000)));
    }
    s_wx.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t suite_weather_start(void)
{
    if (s_wx.task) return ESP_OK;
    if (!s_wx.mtx) {
        s_wx.mtx = xSemaphoreCreateMutex();
        if (!s_wx.mtx) return ESP_ERR_NO_MEM;
    }
    if (!s_wx.wake) {
        s_wx.wake = xSemaphoreCreateBinary();
        if (!s_wx.wake) {
            vSemaphoreDelete(s_wx.mtx);
            s_wx.mtx = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    // 上一次停止可能留下唤醒信号:启动前清掉,避免跳过首轮等待节拍。
    (void)xSemaphoreTake(s_wx.wake, 0);
    suite_wx_body_reset(&s_resp);
    s_wx.run = true;
    if (xTaskCreate(wx_task, "suite_wx", WX_TASK_STACK, NULL, 4, &s_wx.task) !=
        pdPASS) {
        s_wx.run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void suite_weather_stop(void)
{
    s_wx.run = false;
    if (s_wx.wake) (void)xSemaphoreGive(s_wx.wake);  // 唤醒等待,尽快退出
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
    if (!s_wx.wake) return false;
    // 置位唤醒信号量即可:任务在抓取开始前消费本轮请求,抓取过程中到达的
    // 新请求留在信号量里,抓取结束后立即再抓取一次,而不是被清零丢弃。
    (void)xSemaphoreGive(s_wx.wake);
    return true;
}
