// main/suite_connection.c —— Wi-Fi STA + SNTP 设备侧实现。
#include "suite_connection.h"
#include "suite_storage.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <string.h>
#include <time.h>

static const char *TAG = "suite_conn";

#define CONN_QUEUE_DEPTH 8
#define CONN_TASK_STACK 4096
#define CONN_NTP_SERVERS 3

typedef struct {
    suite_conn_machine_t fsm;
    QueueHandle_t queue;
    TaskHandle_t task;
    esp_netif_t *netif;
    suite_time_tracker_t time;
    portMUX_TYPE time_mux;
    volatile suite_conn_state_t state_pub;
    volatile bool sntp_up;
    bool wifi_inited;
    bool wifi_started;
    bool netif_ready;
    bool running;
} conn_ctx_t;

static conn_ctx_t s_conn;

static const char *const k_ntp_hosts[CONN_NTP_SERVERS] = {
    "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov",
};

// esp_event 回调 → 事件队列(满则丢弃并计数,保证回调轻量)。
typedef enum {
    Q_WIFI_STARTED = SUITE_CONN_EV_WIFI_STARTED,
    Q_ASSOC_FAIL = SUITE_CONN_EV_ASSOC_FAIL,
    Q_GOT_IP = SUITE_CONN_EV_GOT_IP,
    Q_LOST = SUITE_CONN_EV_LOST,
    Q_STOP = SUITE_CONN_EV_STOP,
    Q_RETRY_NOW = 100,   // 显式取值,避免与隐式续排的事件值冲突
} queue_ev_t;

static void post_ev(queue_ev_t ev)
{
    if (!s_conn.queue) return;
    (void)xQueueSend(s_conn.queue, &ev, 0);
}

static void time_sync_cb(struct timeval *tv)
{
    portENTER_CRITICAL(&s_conn.time_mux);
    suite_time_sync_succeeded(&s_conn.time, (int64_t)tv->tv_sec);
    portEXIT_CRITICAL(&s_conn.time_mux);
    ESP_LOGI(TAG, "SNTP 校时成功");
}

static void sntp_start(void)
{
    if (s_conn.sntp_up) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    for (int i = 0; i < CONN_NTP_SERVERS; i++) {
        esp_sntp_setservername(i, k_ntp_hosts[i]);
    }
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    s_conn.sntp_up = true;
}

static void sntp_stop(void)
{
    if (!s_conn.sntp_up) return;
    esp_sntp_stop();
    s_conn.sntp_up = false;
}

static void wifi_stack_up(void)
{
    if (!s_conn.netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_conn.netif_ready = true;
    }
    if (!s_conn.netif) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
        s_conn.netif = esp_netif_new(&cfg);
        ESP_ERROR_CHECK(esp_netif_attach_wifi_station(s_conn.netif));
        ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
    }
    if (!s_conn.wifi_inited) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
        s_conn.wifi_inited = true;
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    if (!s_conn.wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_conn.wifi_started = true;
    }
}

static void wifi_stack_down(void)
{
    sntp_stop();
    if (s_conn.wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_conn.wifi_started = false;
    }
}

static esp_err_t wifi_connect(void)
{
    suite_wifi_cred_t cred;
    esp_err_t err = suite_storage_wifi_get(&cred);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "无 Wi-Fi 凭据,等待配网(NVS 无记录)");
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, cred.ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, cred.password, sizeof(wcfg.sta.password));
    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) return err;
    return esp_wifi_connect();
}

// 事件回调:esp_event 任务上下文,只入队。
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case WIFI_EVENT_STA_START:
        post_ev(Q_WIFI_STARTED);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        post_ev(Q_LOST);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP) post_ev(Q_GOT_IP);
}

static void conn_task(void *arg)
{
    (void)arg;
    queue_ev_t ev;
    uint32_t wait_ms = 0;
    TickType_t deadline = 0;

    suite_conn_init(&s_conn.fsm);
    suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_START);
    wifi_stack_up();  // esp_wifi_start() → STA_START → Q_WIFI_STARTED

    for (;;) {
        TickType_t timeout = portMAX_DELAY;
        if (s_conn.fsm.state == SUITE_CONN_RETRY_WAIT) {
            TickType_t now = xTaskGetTickCount();
            timeout = (deadline > now) ? (deadline - now) : 0;
        }
        if (xQueueReceive(s_conn.queue, &ev, timeout) != pdTRUE) {
            ev = Q_RETRY_NOW;  // 退避到期
        }

        switch (ev) {
        case Q_STOP:
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_STOP);
            wifi_stack_down();
            s_conn.running = false;
            vTaskDelete(NULL);
            return;

        case Q_RETRY_NOW:
            if (s_conn.fsm.state == SUITE_CONN_RETRY_WAIT) {
                suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_START);
                if (wifi_connect() != ESP_OK) {
                    suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_ASSOC_FAIL);
                }
            }
            break;

        case Q_WIFI_STARTED:
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_WIFI_STARTED);
            if (wifi_connect() != ESP_OK) {
                // 无凭据:留在 WAIT_IP 语义不对,退回重试等待,慢速轮询。
                suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_ASSOC_FAIL);
            }
            break;

        case Q_ASSOC_FAIL:
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_ASSOC_FAIL);
            break;

        case Q_GOT_IP:
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_GOT_IP);
            portENTER_CRITICAL(&s_conn.time_mux);
            suite_time_sync_started(&s_conn.time);
            portEXIT_CRITICAL(&s_conn.time_mux);
            sntp_start();
            break;

        case Q_LOST:
            sntp_stop();
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_LOST);
            esp_wifi_connect();  // 立即发起;失败会再走 DISCONNECTED→退避
            break;

        default:
            break;
        }

        s_conn.state_pub = s_conn.fsm.state;
        if (s_conn.fsm.state == SUITE_CONN_RETRY_WAIT) {
            wait_ms = suite_conn_backoff_ms(s_conn.fsm.retry_count);
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
            ESP_LOGW(TAG, "连接失败,退避 %lu ms 后重试(第 %lu 次)",
                     (unsigned long)wait_ms, (unsigned long)s_conn.fsm.retry_count);
        }
    }
}

esp_err_t suite_connection_start(void)
{
    if (s_conn.task) return ESP_OK;  // 幂等

    memset(&s_conn, 0, sizeof(s_conn));
    s_conn.time_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    suite_time_tracker_init(&s_conn.time);

    esp_err_t err = suite_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    // 开发引导:Kconfig 提供默认凭据且 NVS 无记录时写入一次(仓库默认为空)。
    if (!suite_storage_wifi_exists() &&
        CONFIG_SUITE_DEFAULT_WIFI_SSID[0] != '\0') {
        suite_wifi_cred_t c = { 0 };
        strlcpy(c.ssid, CONFIG_SUITE_DEFAULT_WIFI_SSID, sizeof(c.ssid));
        strlcpy(c.password, CONFIG_SUITE_DEFAULT_WIFI_PASS, sizeof(c.password));
        (void)suite_storage_wifi_set(&c);
        ESP_LOGI(TAG, "已写入开发引导 Wi-Fi 配置");
    }

    s_conn.queue = xQueueCreate(CONN_QUEUE_DEPTH, sizeof(queue_ev_t));
    if (!s_conn.queue) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    if (xTaskCreate(conn_task, "suite_conn", CONN_TASK_STACK, NULL, 5,
                    &s_conn.task) != pdPASS) {
        vQueueDelete(s_conn.queue);
        s_conn.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_conn.running = true;
    return ESP_OK;
}

void suite_connection_stop(void)
{
    if (s_conn.task && s_conn.running) post_ev(Q_STOP);
}

suite_conn_state_t suite_connection_state(void)
{
    return s_conn.state_pub;
}

int64_t suite_connection_now(void)
{
    return (int64_t)time(NULL);
}

const suite_time_tracker_t *suite_connection_time(void)
{
    return &s_conn.time;
}

void suite_connection_reconnect_now(void)
{
    post_ev(Q_RETRY_NOW);
}
