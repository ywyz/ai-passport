// main/suite_prov_ble.c —— 手机扫码 BLE 配网:NimBLE GATT 服务实现。
//
// 职责划分:
//   - NimBLE host 在应用启动后初始化一次,常驻;广播只在配网页打开期间存在。
//   - GATT/GAP 回调运行在 NimBLE host 任务,只把入站帧/连接事件复制入队,
//     不做任何阻塞工作(JSON 解析、Wi-Fi 扫描、NVS、notify 都在 prov 任务)。
//   - prov 任务负责配对码校验、分包重组、命令处理、状态事件主动推送。
//
// 安全基线:链路层 Just Works,不防窃听/中间人;授权仅靠屏幕 6 位配对码
// (连续错误 3 次主动断开),属于基础防护而非完整授权设计。日志禁止打印
// Wi-Fi 密码;配对码只出现在设备屏幕上,不进日志。
#include "suite_prov_ble.h"

#include "suite_city_data.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_prov_proto.h"
#include "suite_storage.h"
#include "suite_weather.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "suite_prov";

// ---- 固定 UUID(与 provisioning/web/app.js 严格一致) ----
// 规范短写 7a11-000X-4f1a-8c20-50a7b6e9000X 的 128 位展开为
// 7a11000X-4f1a-8c20-50a7-b6e90000000X(BLE_UUID128_INIT 按小端填字节)。
static const ble_uuid128_t k_uuid_svc = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0xe9, 0xb6, 0xa7, 0x50,
    0x20, 0x8c, 0x1a, 0x4f, 0x01, 0x00, 0x11, 0x7a);
static const ble_uuid128_t k_uuid_rx = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0xe9, 0xb6, 0xa7, 0x50,
    0x20, 0x8c, 0x1a, 0x4f, 0x02, 0x00, 0x11, 0x7a);
static const ble_uuid128_t k_uuid_tx = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0xe9, 0xb6, 0xa7, 0x50,
    0x20, 0x8c, 0x1a, 0x4f, 0x03, 0x00, 0x11, 0x7a);
static const ble_uuid128_t k_uuid_info = BLE_UUID128_INIT(
    0x04, 0x00, 0x00, 0x00, 0xe9, 0xb6, 0xa7, 0x50,
    0x20, 0x8c, 0x1a, 0x4f, 0x04, 0x00, 0x11, 0x7a);

#define DEV_NAME_PREFIX "FoloPassport-"
#define DEV_NAME_LEN (sizeof(DEV_NAME_PREFIX) - 1 + 4)  // 不含结尾 '\0'

// ---- 模块全局状态 ----
static bool s_inited;
static bool s_hs_synced;
static uint8_t s_addr_type;
static char s_device_id[5];
static char s_dev_name[DEV_NAME_LEN + 1];
static char s_info_json[72];

// 配网页会话:广播/连接/任务只在 begin..end 之间存在。
static volatile bool s_session;
static volatile suite_prov_phase_t s_phase;
static QueueHandle_t s_pq;
static TaskHandle_t s_ptask;
static SemaphoreHandle_t s_task_done;
static char s_expected_pin[SUITE_PROV_PIN_LEN + 1];

static uint16_t s_tx_val_handle;

// ---- prov 任务队列项 ----
enum {
    PQ_RX = 1,        // 手机写入的一帧;a=len,data=帧字节
    PQ_CONNECT,       // a=conn_handle
    PQ_DISCONNECT,    // a=conn_handle
    PQ_SUBSCRIBE,     // a=1 使能通知 / 0 关闭
    PQ_MTU,           // a=协商后 MTU
    PQ_STOP,          // 配网页退出,任务收尾自删
};

typedef struct {
    uint8_t type;
    uint16_t a;
    uint8_t data[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];
} pq_item_t;

#define PROV_QUEUE_DEPTH 8
#define PROV_TASK_STACK 5120
#define PROV_TASK_PRIO 5
#define SCAN_MAX_HINTS 40
#define SCAN_REPLY_MAX 20

// ---- 前向声明 ----
static int gap_event(struct ble_gap_event *event, void *arg);
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 手机 → 设备:支持 Write 与 Write Without Response。
                .uuid = &k_uuid_rx.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 设备 → 手机:Notify。
                .uuid = &k_uuid_tx.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            {
                // 静态设备信息 {"id":"XXXX","fw":"..."}。
                .uuid = &k_uuid_info.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

// ---------------- 广播 ----------------

static int advertise_start(void)
{
    // 广播包:Flags + 完整 128-bit 服务 UUID(3+18=21 字节,保证
    // Web Bluetooth 按 services 过滤一定命中)。
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &k_uuid_svc;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    // 扫描响应:完整设备名(广播包已放不下)。
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_dev_name;
    rsp.name_len = strlen(s_dev_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params,
                             gap_event, NULL);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址不可用: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断 BLE 地址类型失败: %d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    rc = ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    if (rc == 0) {
        snprintf(s_device_id, sizeof(s_device_id), "%02X%02X",
                 addr[4], addr[5]);
        snprintf(s_dev_name, sizeof(s_dev_name), DEV_NAME_PREFIX "%s",
                 s_device_id);
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(s_info_json, sizeof(s_info_json),
                 "{\"id\":\"%s\",\"fw\":\"%s\"}", s_device_id,
                 app ? app->version : "0");
        ble_svc_gap_device_name_set(s_dev_name);
        ESP_LOGI(TAG, "BLE 设备 ID: %s", s_device_id);  // 不含配对码/密码
    }
    s_hs_synced = true;
    if (s_session) {
        rc = advertise_start();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "配网广播启动失败: %d", rc);
        }
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host 复位: %d", reason);
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------- GATT/GAP 回调(只入队) ----------------

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (ble_uuid_cmp(ctxt->chr->uuid, &k_uuid_info.u) == 0) {
            return os_mbuf_append(ctxt->om, s_info_json,
                                  strlen(s_info_json)) == 0
                       ? 0
                       : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        if (ble_uuid_cmp(ctxt->chr->uuid, &k_uuid_rx.u) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (!s_pq) return BLE_ATT_ERR_UNLIKELY;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < SUITE_PROV_HEADER_LEN ||
            len > SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        pq_item_t item = {0};
        item.type = PQ_RX;
        item.a = len;
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, item.data, len, &copied);
        if (rc != 0 || copied != len) return BLE_ATT_ERR_UNLIKELY;
        // 队列满:返回资源不足,让手机端感知背压后重试。
        if (xQueueSend(s_pq, &item, 0) != pdTRUE) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    default:
        // 本服务没有可写描述符;其它操作一律拒绝。
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    pq_item_t item = {0};
    bool enqueue = false;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_phase = SUITE_PROV_PHASE_CONNECTED;
            item.type = PQ_CONNECT;
            item.a = event->connect.conn_handle;
            enqueue = true;
            ESP_LOGI(TAG, "手机已连接");
        } else if (s_session) {
            // 连接建立失败:回到可连接广播。
            (void)advertise_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "手机断开: %d", event->disconnect.reason);
        item.type = PQ_DISCONNECT;
        item.a = event->disconnect.conn.conn_handle;
        enqueue = true;
        if (s_session) {
            s_phase = SUITE_PROV_PHASE_ADVERTISING;
            (void)advertise_start();
        } else {
            s_phase = SUITE_PROV_PHASE_IDLE;
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            item.type = PQ_SUBSCRIBE;
            item.a = event->subscribe.cur_notify ? 1 : 0;
            enqueue = true;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %d", event->mtu.value);
        item.type = PQ_MTU;
        item.a = event->mtu.value;
        enqueue = true;
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 连接建立会结束广播,属正常;其它原因且仍在会话中则重新广播。
        if (s_session && event->adv_complete.reason == BLE_HS_ETIMEOUT) {
            (void)advertise_start();
        }
        break;

    default:
        break;
    }

    if (enqueue && s_pq) (void)xQueueSend(s_pq, &item, 0);
    return 0;
}

// ---------------- 一次性初始化 ----------------

esp_err_t suite_prov_ble_init(void)
{
    if (s_inited) return ESP_OK;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Just Works 默认配置(不改 sm_io_cap/bonding/mitm):授权由应用层配对码
    // 兜底。这是基础防护,不是完整授权设计。
    (void)ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(k_svcs);
    if (rc != 0) return ESP_FAIL;
    rc = ble_gatts_add_svcs(k_svcs);
    if (rc != 0) return ESP_FAIL;

    nimble_port_freertos_init(nimble_host_task);
    s_inited = true;
    ESP_LOGI(TAG, "NimBLE 配网服务就绪,空闲堆 %lu 字节",
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

bool suite_prov_ble_device_id(char out[5])
{
    if (!s_hs_synced) return false;
    memcpy(out, s_device_id, 5);
    return true;
}

// ---------------- prov 任务:状态 JSON 与事件 ----------------

// 状态变化签名(避免每 500ms 重复 notify 同一份状态)。
typedef struct {
    uint8_t wifi;
    uint16_t reason;
    uint8_t has_ip;
    char ip[16];
    int8_t rssi_bucket;
    uint8_t time_synced;
    char city_name[32];
    int32_t city_la;
    int32_t city_lo;
    char ssid_mask[8];
} status_sig_t;

static const char *wifi_state_name(suite_conn_state_t st)
{
    switch (st) {
    case SUITE_CONN_ONLINE: return "online";
    case SUITE_CONN_CONNECTING:
    case SUITE_CONN_WAIT_IP: return "connecting";
    case SUITE_CONN_RETRY_WAIT:
        // 没有任何凭据时不叫"失败",而是待配网的 idle。
        return suite_storage_wifi_exists() ? "fail" : "idle";
    default:
        return "idle";
    }
}

// 把 data 填成 status 事件/应答共用的数据对象;同时产出变化签名。
static cJSON *build_status_data(status_sig_t *sig)
{
    suite_conn_state_t st = suite_connection_state();
    const char *ws = wifi_state_name(st);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "wifi", ws);

    // SSID 掩码:仅保留前两个字符,其余以 *** 代替,不回传完整网络名。
    suite_wifi_cred_t cred;
    char mask[8] = {0};
    if (suite_storage_wifi_get(&cred) == ESP_OK) {
        size_t n = strlen(cred.ssid);
        if (n <= 2) {
            strlcpy(mask, "***", sizeof(mask));
        } else {
            snprintf(mask, sizeof(mask), "%c%c***", cred.ssid[0],
                     cred.ssid[1]);
        }
        cJSON_AddStringToObject(data, "ssid", mask);
    }

    char ip[16] = {0};
    bool has_ip = suite_connection_ip_str(ip, sizeof(ip));
    if (has_ip) cJSON_AddStringToObject(data, "ip", ip);

    int rssi = 0;
    if (suite_connection_ap_rssi(&rssi)) {
        cJSON_AddNumberToObject(data, "rssi", rssi);
    }

    if (strcmp(ws, "fail") == 0) {
        uint16_t reason = suite_connection_last_reason();
        cJSON_AddNumberToObject(data, "reason", reason);
    }

    const suite_time_tracker_t *tt = suite_connection_time();
    bool synced = suite_time_is_trusted(tt, suite_connection_now());
    cJSON_AddBoolToObject(data, "time_synced", synced);

    suite_city_t city;
    suite_storage_city_get(&city);
    cJSON *cj = cJSON_CreateObject();
    cJSON_AddStringToObject(cj, "n", city.name);
    cJSON_AddNumberToObject(cj, "la", city.lat_x1e4);
    cJSON_AddNumberToObject(cj, "lo", city.lon_x1e4);
    cJSON_AddItemToObject(data, "city", cj);

    if (sig) {
        memset(sig, 0, sizeof(*sig));
        sig->wifi = (uint8_t)st;
        sig->reason = suite_connection_last_reason();
        sig->has_ip = has_ip;
        strlcpy(sig->ip, ip, sizeof(sig->ip));
        sig->rssi_bucket = (int8_t)(rssi / 5);
        sig->time_synced = synced ? 1 : 0;
        strlcpy(sig->city_name, city.name, sizeof(sig->city_name));
        sig->city_la = city.lat_x1e4;
        sig->city_lo = city.lon_x1e4;
        strlcpy(sig->ssid_mask, mask, sizeof(sig->ssid_mask));
    }
    return data;
}

static void tx_json(uint16_t conn, bool subscribed, uint16_t mtu,
                    const char *text)
{
    if (!subscribed || conn == BLE_HS_CONN_HANDLE_NONE) return;

    size_t payload = (size_t)mtu - SUITE_PROV_HEADER_LEN - 3 /* ATT 头 */;
    if (payload < SUITE_PROV_PAYLOAD_MIN) payload = SUITE_PROV_PAYLOAD_MIN;
    if (payload > SUITE_PROV_PAYLOAD_MAX) payload = SUITE_PROV_PAYLOAD_MAX;

    size_t total = strlen(text);
    size_t frames = suite_prov_frame_count(total, payload);
    uint8_t buf[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];
    for (size_t i = 0; i < frames; i++) {
        size_t flen = suite_prov_frame_build((const uint8_t *)text, total,
                                             payload, i, buf, sizeof(buf));
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)flen);
        if (!om) {
            ESP_LOGW(TAG, "notify mbuf 分配失败,丢弃剩余帧");
            return;
        }
        int rc = ble_gatts_notify_custom(conn, s_tx_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify 发送失败: %d", rc);
            return;
        }
        // 给控制器留出排空时间,避免突发多帧时协议栈缓冲溢出。
        if (i + 1 < frames) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static char *s_print(cJSON *root)
{
    return cJSON_PrintUnformatted(root);
}

static void reply_ok(uint16_t conn, bool subscribed, uint16_t mtu,
                     double id, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) cJSON_AddItemToObject(root, "data", data);
    char *text = s_print(root);
    if (text) tx_json(conn, subscribed, mtu, text);
    free(text);
    cJSON_Delete(root);
}

static void reply_err(uint16_t conn, bool subscribed, uint16_t mtu,
                      double id, const char *err)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "err", err);
    char *text = s_print(root);
    if (text) tx_json(conn, subscribed, mtu, text);
    free(text);
    cJSON_Delete(root);
}

static void push_status(uint16_t conn, bool subscribed, uint16_t mtu)
{
    status_sig_t sig;
    cJSON *data = build_status_data(&sig);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ev", "status");
    cJSON_AddItemToObject(root, "data", data);
    char *text = s_print(root);
    if (text) tx_json(conn, subscribed, mtu, text);
    free(text);
    cJSON_Delete(root);
}

// ---------------- 命令处理 ----------------

static void cmd_wifi_scan(double id, uint16_t conn, bool subscribed,
                          uint16_t mtu)
{
    wifi_scan_config_t scfg = {0};
    scfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scfg.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 扫描启动失败: %s", esp_err_to_name(err));
        reply_err(conn, subscribed, mtu, id, "scan_busy");
        return;
    }

    uint16_t n = SCAN_MAX_HINTS;
    wifi_ap_record_t *aps = calloc(n, sizeof(wifi_ap_record_t));
    if (!aps) {
        reply_err(conn, subscribed, mtu, id, "no_mem");
        return;
    }
    esp_wifi_scan_get_ap_records(&n, aps);

    // 同名去重取最强,用独立堆表避免大数组上栈。
    typedef struct {
        char ssid[33];
        int8_t rssi;
        uint8_t sec;
        bool used;
    } scan_ent_t;
    scan_ent_t *ents = calloc(n, sizeof(scan_ent_t));
    cJSON *nets = cJSON_CreateArray();
    if (ents && nets) {
        int count = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (aps[i].ssid[0] == '\0') continue;  // 隐藏网络不给空名
            int slot = -1;
            for (int j = 0; j < count; j++) {
                if (strcmp(ents[j].ssid, (char *)aps[i].ssid) == 0) {
                    slot = j;
                    break;
                }
            }
            if (slot < 0) {
                if (count >= (int)n) continue;
                slot = count++;
                strlcpy(ents[slot].ssid, (char *)aps[i].ssid,
                        sizeof(ents[slot].ssid));
                ents[slot].rssi = aps[i].rssi;
                ents[slot].sec = (uint8_t)aps[i].authmode;
                ents[slot].used = true;
            } else if (aps[i].rssi > ents[slot].rssi) {
                ents[slot].rssi = aps[i].rssi;
                ents[slot].sec = (uint8_t)aps[i].authmode;
            }
        }
        // 按信号从强到弱简单插入排序,取前 20。
        for (int i = 1; i < count; i++) {
            scan_ent_t tmp = ents[i];
            int j = i - 1;
            while (j >= 0 && ents[j].rssi < tmp.rssi) {
                ents[j + 1] = ents[j];
                j--;
            }
            ents[j + 1] = tmp;
        }
        int out_n = count > SCAN_REPLY_MAX ? SCAN_REPLY_MAX : count;
        for (int i = 0; i < out_n; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", ents[i].ssid);
            cJSON_AddNumberToObject(net, "rssi", ents[i].rssi);
            // sec 直接使用 WIFI_AUTH_* 枚举序号,网页据此显示锁标。
            cJSON_AddNumberToObject(net, "sec", ents[i].sec);
            cJSON_AddItemToArray(nets, net);
        }
    }
    free(aps);
    free(ents);

    if (!nets) {
        reply_err(conn, subscribed, mtu, id, "no_mem");
        return;
    }
    reply_ok(conn, subscribed, mtu, id, nets);
}

static void cmd_provinces(double id, uint16_t conn, bool subscribed,
                          uint16_t mtu)
{
    cJSON *arr = cJSON_CreateArray();
    int n = suite_city_province_count();
    for (int i = 0; i < n; i++) {
        const suite_province_def_t *p = suite_city_province_at(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "n", p->name);
        cJSON_AddItemToArray(arr, o);
    }
    reply_ok(conn, subscribed, mtu, id, arr);
}

static void cmd_cities(const cJSON *root, double id, uint16_t conn,
                       bool subscribed, uint16_t mtu)
{
    cJSON *jp = cJSON_GetObjectItem(root, "p");
    if (!cJSON_IsNumber(jp)) {
        reply_err(conn, subscribed, mtu, id, "bad_arg");
        return;
    }
    int p = jp->valueint;
    const suite_province_def_t *prov = suite_city_province_at(p);
    if (!prov) {
        reply_err(conn, subscribed, mtu, id, "bad_arg");
        return;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < prov->city_count; i++) {
        const suite_city_def_t *c = &prov->cities[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "n", c->name);
        cJSON_AddNumberToObject(o, "la", c->lat_x1e4);
        cJSON_AddNumberToObject(o, "lo", c->lon_x1e4);
        cJSON_AddItemToArray(arr, o);
    }
    reply_ok(conn, subscribed, mtu, id, arr);
}

static void handle_message(const uint8_t *msg, size_t len,
                           suite_prov_pin_t *pin, uint16_t conn,
                           bool subscribed, uint16_t mtu, bool *force_status)
{
    cJSON *root = cJSON_ParseWithLength((const char *)msg, len);
    if (!root) {
        reply_err(conn, subscribed, mtu, 0, "bad_json");
        return;
    }

    cJSON *jid = cJSON_GetObjectItem(root, "id");
    double id = cJSON_IsNumber(jid) ? jid->valuedouble : 0;
    cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(jcmd)) {
        reply_err(conn, subscribed, mtu, id, "bad_arg");
        cJSON_Delete(root);
        return;
    }
    const char *cmd = jcmd->valuestring;

    if (strcmp(cmd, "hello") == 0) {
        cJSON *jpin = cJSON_GetObjectItem(root, "pin");
        if (!cJSON_IsString(jpin)) {
            reply_err(conn, subscribed, mtu, id, "bad_pin");
            cJSON_Delete(root);
            return;
        }
        suite_prov_pin_result_t pr =
            suite_prov_pin_verify(pin, jpin->valuestring,
                                  strlen(jpin->valuestring));
        if (pr == SUITE_PROV_PIN_OK) {
            s_phase = SUITE_PROV_PHASE_AUTHED;
            reply_ok(conn, subscribed, mtu, id, cJSON_CreateObject());
            *force_status = true;
        } else {
            reply_err(conn, subscribed, mtu, id, "bad_pin");
            if (pr == SUITE_PROV_PIN_LOCKED) {
                ESP_LOGW(TAG, "配对码连续错误,断开手机");
                // 配对码不得出现在日志;仅记录断开动作。
                (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        cJSON_Delete(root);
        return;
    }

    // 除 hello 外的命令都必须先通过配对码授权。
    if (!suite_prov_pin_authed(pin)) {
        reply_err(conn, subscribed, mtu, id, "need_pin");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        reply_ok(conn, subscribed, mtu, id, build_status_data(NULL));
    } else if (strcmp(cmd, "wifi_scan") == 0) {
        cmd_wifi_scan(id, conn, subscribed, mtu);
    } else if (strcmp(cmd, "provinces") == 0) {
        cmd_provinces(id, conn, subscribed, mtu);
    } else if (strcmp(cmd, "cities") == 0) {
        cmd_cities(root, id, conn, subscribed, mtu);
    } else if (strcmp(cmd, "wifi_set") == 0) {
        cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *jpass = cJSON_GetObjectItem(root, "pass");
        if (!cJSON_IsString(jssid) ||
            strlen(jssid->valuestring) == 0 ||
            strlen(jssid->valuestring) >= sizeof(((suite_wifi_cred_t *)0)
                                                ->ssid) ||
            !cJSON_IsString(jpass) ||
            strlen(jpass->valuestring) > sizeof(((suite_wifi_cred_t *)0)
                                                ->password) - 1) {
            reply_err(conn, subscribed, mtu, id, "bad_arg");
        } else {
            suite_wifi_cred_t cred = {0};
            strlcpy(cred.ssid, jssid->valuestring, sizeof(cred.ssid));
            strlcpy(cred.password, jpass->valuestring, sizeof(cred.password));
            esp_err_t err = suite_storage_wifi_set(&cred);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi 凭据写 NVS 失败: %s",
                         esp_err_to_name(err));
                reply_err(conn, subscribed, mtu, id, "storage");
            } else {
                err = suite_connection_apply_new_wifi();
                if (err != ESP_OK) {
                    reply_err(conn, subscribed, mtu, id, "conn_busy");
                } else {
                    ESP_LOGI(TAG, "新 Wi-Fi 凭据已保存并应用(SSID 长度 %d)",
                             (int)strlen(cred.ssid));  // 不打印 SSID/密码
                    *force_status = true;
                    reply_ok(conn, subscribed, mtu, id, cJSON_CreateObject());
                }
            }
        }
    } else if (strcmp(cmd, "city_set") == 0) {
        cJSON *jp = cJSON_GetObjectItem(root, "p");
        cJSON *jc = cJSON_GetObjectItem(root, "c");
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jc)) {
            reply_err(conn, subscribed, mtu, id, "bad_arg");
        } else {
            const suite_province_def_t *prov =
                suite_city_province_at(jp->valueint);
            int ci = jc->valueint;
            if (!prov || ci < 0 || ci >= prov->city_count) {
                reply_err(conn, subscribed, mtu, id, "bad_arg");
            } else {
                const suite_city_def_t *d = &prov->cities[ci];
                suite_city_t city = {0};
                strlcpy(city.name, d->name, sizeof(city.name));
                city.lat_x1e4 = d->lat_x1e4;
                city.lon_x1e4 = d->lon_x1e4;
                city.tz_off_s = 8 * 3600;
                esp_err_t err = suite_storage_city_set(&city);
                if (err != ESP_OK) {
                    reply_err(conn, subscribed, mtu, id, "storage");
                } else {
                    (void)suite_weather_request_refresh();
                    *force_status = true;
                    reply_ok(conn, subscribed, mtu, id,
                             cJSON_CreateObject());
                }
            }
        }
    } else {
        reply_err(conn, subscribed, mtu, id, "bad_cmd");
    }

    cJSON_Delete(root);
}

// ---------------- prov 任务主体 ----------------

static void prov_task(void *arg)
{
    (void)arg;
    suite_prov_pin_t pin;
    suite_prov_pin_start(&pin, s_expected_pin);

    static uint8_t msg_buf[SUITE_PROV_MSG_MAX];  // 静态,避开任务栈
    suite_prov_reasm_t reasm;
    suite_prov_reasm_init(&reasm, msg_buf, sizeof(msg_buf));

    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    bool subscribed = false;
    uint16_t mtu = 23;
    bool force_status = true;
    status_sig_t last_sig;
    memset(&last_sig, 0, sizeof(last_sig));

    // CONFIG_FREERTOS_HZ=1000,故 tick 计数即毫秒数。
    TickType_t last_status = xTaskGetTickCount();

    for (;;) {
        pq_item_t item;
        bool got = xQueueReceive(s_pq, &item, pdMS_TO_TICKS(200));

        if (got) {
            switch (item.type) {
            case PQ_STOP:
                // 通知 end() 后阻塞在此:由 end() 确定性地 vTaskDelete 本任务,
                // 避免"自删与队列删除并发"的时序竞争。
                xSemaphoreGive(s_task_done);
                for (;;) {
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
                return;

            case PQ_CONNECT:
                conn = item.a;
                subscribed = false;
                suite_prov_pin_start(&pin, s_expected_pin);
                suite_prov_reasm_init(&reasm, msg_buf, sizeof(msg_buf));
                force_status = true;
                break;

            case PQ_DISCONNECT:
                if (conn == item.a || conn == BLE_HS_CONN_HANDLE_NONE) {
                    conn = BLE_HS_CONN_HANDLE_NONE;
                    subscribed = false;
                    suite_prov_reasm_init(&reasm, msg_buf, sizeof(msg_buf));
                }
                break;

            case PQ_SUBSCRIBE:
                subscribed = item.a != 0;
                if (subscribed) force_status = true;
                break;

            case PQ_MTU:
                mtu = item.a;
                break;

            case PQ_RX: {
                const uint8_t *out = NULL;
                size_t out_len = 0;
                int r = suite_prov_reasm_feed(
                    &reasm, item.data, item.a,
                    (uint32_t)xTaskGetTickCount(), &out, &out_len);
                if (r > 0) {
                    handle_message(out, out_len, &pin, conn, subscribed,
                                   mtu, &force_status);
                } else if (r < 0) {
                    // 非法帧:静默丢弃半条消息,等下一个 START 重开。
                    ESP_LOGD(TAG, "收到非法配网帧,重组会话已重置");
                }
                break;
            }

            default:
                break;
            }
        }

        // 半条消息 3s 超时清理。
        (void)suite_prov_reasm_tick(&reasm, (uint32_t)xTaskGetTickCount());

        // 每 500ms 检测一次状态变化,变化时主动 notify(连接 GOT_IP、
        // 断开带 reason、SNTP 同步、城市保存等都经此通道触达手机)。
        TickType_t now = xTaskGetTickCount();
        if (subscribed && conn != BLE_HS_CONN_HANDLE_NONE &&
            (force_status || now - last_status >= pdMS_TO_TICKS(500))) {
            last_status = now;
            status_sig_t sig;
            cJSON *tmp = build_status_data(&sig);
            cJSON_Delete(tmp);
            if (force_status || memcmp(&sig, &last_sig, sizeof(sig)) != 0) {
                push_status(conn, subscribed, mtu);
                last_sig = sig;
            }
            force_status = false;
        }
    }
}

// ---------------- 配网页生命周期 ----------------

esp_err_t suite_prov_ble_begin(const char pin[6])
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_ptask) return ESP_ERR_INVALID_STATE;
    if (!pin) return ESP_ERR_INVALID_ARG;

    memcpy(s_expected_pin, pin, SUITE_PROV_PIN_LEN);
    s_expected_pin[SUITE_PROV_PIN_LEN] = '\0';

    s_task_done = xSemaphoreCreateBinary();
    s_pq = xQueueCreate(PROV_QUEUE_DEPTH, sizeof(pq_item_t));
    if (!s_task_done || !s_pq) {
        if (s_task_done) vSemaphoreDelete(s_task_done);
        if (s_pq) vQueueDelete(s_pq);
        s_task_done = NULL;
        s_pq = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_session = true;
    s_phase = SUITE_PROV_PHASE_ADVERTISING;
    if (xTaskCreate(prov_task, "suite_prov", PROV_TASK_STACK, NULL,
                    PROV_TASK_PRIO, &s_ptask) != pdPASS) {
        s_session = false;
        s_phase = SUITE_PROV_PHASE_IDLE;
        vSemaphoreDelete(s_task_done);
        vQueueDelete(s_pq);
        s_task_done = NULL;
        s_pq = NULL;
        s_ptask = NULL;
        return ESP_ERR_NO_MEM;
    }

    // host 早已同步时直接开广播;未同步则由 on_sync 在同步后开启。
    if (s_hs_synced) {
        int rc = advertise_start();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "配网广播启动失败: %d", rc);
            suite_prov_ble_end();
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "进入配网页,广播中,空闲堆 %lu 字节",
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

void suite_prov_ble_end(void)
{
    if (!s_ptask) {
        s_session = false;
        s_phase = SUITE_PROV_PHASE_IDLE;
        return;
    }

    s_session = false;
    s_phase = SUITE_PROV_PHASE_IDLE;
    (void)ble_gap_adv_stop();
    // 没有直接保存连接句柄的全局;广播停止后主动断开由任务会话内的
    // 连接自然超时不是我们想要的,故遍历当前连接断开。NimBLE 单连接配置下
    // 直接对句柄 0 断开(单 peripheral 连接句柄恒为 0)。
    (void)ble_gap_terminate(0, BLE_ERR_REM_USER_CONN_TERM);

    pq_item_t stop = {0};
    stop.type = PQ_STOP;
    (void)xQueueSend(s_pq, &stop, 0);
    // 任务收到 PQ_STOP 并交出信号量后即让出,由这里统一删除,
    // 保证删队列时没有任务仍在使用它。
    if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "配网任务退出超时,强制删除");
    }
    vTaskDelete(s_ptask);
    s_ptask = NULL;
    vQueueDelete(s_pq);
    s_pq = NULL;
    vSemaphoreDelete(s_task_done);
    s_task_done = NULL;
}

void suite_prov_ble_snapshot(suite_prov_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->phase = s_phase;
    out->wifi = suite_connection_state();
    out->reason = suite_connection_last_reason();
    out->has_ip = suite_connection_ip_str(out->ip, sizeof(out->ip));
    (void)suite_connection_ap_rssi(&out->rssi);
    const suite_time_tracker_t *tt = suite_connection_time();
    out->time_synced =
        suite_time_is_trusted(tt, suite_connection_now());
    suite_city_t city;
    if (suite_storage_city_get(&city) == ESP_OK) {
        strlcpy(out->city, city.name, sizeof(out->city));
    }
}
