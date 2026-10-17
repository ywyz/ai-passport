// main/suite_prov_ble.c —— 手机扫码 BLE 配网:NimBLE GATT 服务实现。
//
// 职责划分:
//   - NimBLE host 在应用启动后初始化一次,常驻同步;广播只在配网页打开
//     期间存在。BT 控制器常开不 disable:Wi-Fi 共存由 CONFIG_BT_CTRL_MODEM_SLEEP
//     (BLE 事件间睡眠)+ 关闭软件共存调度(硬件仲裁)保障,disable/enable
//     循环救不了共存还会让 host 丢失同步态(host 自认 GOOD,enable 后
//     无事件触发重新 sync,广播/设备 ID 永不恢复)。
//   - GATT/GAP 回调运行在 NimBLE host 任务,只把入站帧/连接事件复制入队,
//     并在极短临界区内同步链路代次快照;不做任何阻塞工作(JSON 解析、
//     Wi-Fi 扫描、NVS、notify 都在 prov 任务)。
//   - prov 任务负责配对码校验、分包重组、命令处理、状态事件主动推送;
//     每轮循环按链路代次对齐会话,断连/换机时旧授权、订阅、MTU 与半条
//     消息全部立即失效(队列满丢事件也可靠代次恢复)。
//   - 代次(gen)在每次连接建立/断开时自增:仅比较 conn_handle 无法区分
//     手机的断开与同句柄重连。终校验与通知提交处于同一串行化边界
//     (s_link_lock,见下方说明):阻塞扫描返回、逐帧发送、校验通过到
//     提交之间的链路切换都不会把旧会话数据发给新连接。
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

// ---- 链路代次与链路状态 ----
// GAP/GATT 回调是唯一写者,代次在每次连接建立/断开时自增;prov 任务以
// 快照读取。订阅与 MTU 是"当前链路"的属性,由回调先于队列事件更新,
// 因此即使队列满导致事件被丢弃,下一次对齐也能恢复且不会继承旧连接。
typedef struct {
    uint32_t gen;        // 链路代次:每次连接/断开自增
    uint16_t conn;       // 当前连接句柄;NONE 表示无连接
    bool subscribed;     // 当前链路是否已订阅 TX 通知
    uint16_t mtu;        // 当前链路协商的 ATT MTU(未协商为默认 23)
} prov_link_t;

static portMUX_TYPE s_link_mux = portMUX_INITIALIZER_UNLOCKED;
static prov_link_t s_link = {
    .gen = 0,
    .conn = BLE_HS_CONN_HANDLE_NONE,
    .subscribed = false,
    .mtu = SUITE_PROV_MTU_DEFAULT,
};

// 发送串行化边界(init 期创建,进程级常驻;begin/end 不重建)。
//
// 为什么需要它:句柄会被协议栈复用 —— A 断开、B 以同一句柄连接后,
// ble_gatts_notify_custom() 会在提交时按句柄解析到 B。只在提交前"再查
// 一次代次"无法消除窗口:GAP 回调是另一个任务(host 任务),校验与提交
// 之间它仍可推进代次。
//
// 边界语义(与 NimBLE 连接生命周期处理顺序一致):
//   - 连接/断开 GAP 回调推进代次前取本锁(NimBLE 先删除旧连接表项、
//     后派发断开事件);
//   - 提交一帧时持锁完成"按当前代次校验 + ble_gatts_notify_custom";
//   - 因此持锁期间按句柄解析到的只可能是旧连接(帧仍归旧会话),或者
//     旧连接已删除而新连接尚未插入(提交返回错误并释放 mbuf);
//   - 新连接的 CONNECT 回调同样要取锁推进代次,持锁期间代次不会变化。
// 只在任务上下文取用(prov 任务与 GAP 回调);持锁区间不含阻塞 I/O,
// 也不在 FreeRTOS 临界区/自旋锁内调用协议栈接口。
static SemaphoreHandle_t s_link_lock;

// 若锁不可用(init 未完成),退化为"不串行化"的最小行为,避免崩溃。
static void link_lock_take(void)
{
    if (s_link_lock) (void)xSemaphoreTake(s_link_lock, portMAX_DELAY);
}

static void link_lock_give(void)
{
    if (s_link_lock) (void)xSemaphoreGive(s_link_lock);
}

// 任意任务上下文可读的链路快照(临界区只做字段拷贝,不调用阻塞接口)。
static prov_link_t link_snapshot(void)
{
    prov_link_t out;
    portENTER_CRITICAL(&s_link_mux);
    out = s_link;
    portEXIT_CRITICAL(&s_link_mux);
    return out;
}

static uint32_t link_gen_now(void)
{
    uint32_t gen;
    portENTER_CRITICAL(&s_link_mux);
    gen = s_link.gen;
    portEXIT_CRITICAL(&s_link_mux);
    return gen;
}

// 以下更新函数只由 GAP 回调调用,返回更新后的代次。
// 连接/断开推进代次前取发送串行化边界:提交持锁期间不得换机。
static uint32_t link_on_connected(uint16_t conn)
{
    uint32_t gen;
    link_lock_take();
    portENTER_CRITICAL(&s_link_mux);
    s_link.gen++;
    s_link.conn = conn;
    s_link.subscribed = false;           // 新连接不继承旧订阅
    s_link.mtu = SUITE_PROV_MTU_DEFAULT; // 也不继承旧 MTU
    gen = s_link.gen;
    portEXIT_CRITICAL(&s_link_mux);
    link_lock_give();
    return gen;
}

static uint32_t link_on_disconnected(uint16_t conn)
{
    uint32_t gen;
    link_lock_take();
    portENTER_CRITICAL(&s_link_mux);
    s_link.gen++;
    if (s_link.conn == conn || s_link.conn == BLE_HS_CONN_HANDLE_NONE) {
        s_link.conn = BLE_HS_CONN_HANDLE_NONE;
        s_link.subscribed = false;
        s_link.mtu = SUITE_PROV_MTU_DEFAULT;
    }
    gen = s_link.gen;
    portEXIT_CRITICAL(&s_link_mux);
    link_lock_give();
    return gen;
}

// host 异常复位:连接表被清空且不会再派发断开事件,必须在这里作废全部
// 会话身份,否则复用句柄的新连接可能被当成旧会话。
static void link_on_reset(void)
{
    link_lock_take();
    portENTER_CRITICAL(&s_link_mux);
    s_link.gen++;
    s_link.conn = BLE_HS_CONN_HANDLE_NONE;
    s_link.subscribed = false;
    s_link.mtu = SUITE_PROV_MTU_DEFAULT;
    portEXIT_CRITICAL(&s_link_mux);
    link_lock_give();
}

static void link_on_subscribed(uint16_t conn, bool on)
{
    portENTER_CRITICAL(&s_link_mux);
    if (s_link.conn == conn) s_link.subscribed = on;
    portEXIT_CRITICAL(&s_link_mux);
}

static void link_on_mtu(uint16_t conn, uint16_t mtu)
{
    if (mtu < SUITE_PROV_MTU_DEFAULT) return;
    portENTER_CRITICAL(&s_link_mux);
    if (s_link.conn == conn) s_link.mtu = mtu;
    portEXIT_CRITICAL(&s_link_mux);
}

static uint16_t s_tx_val_handle;

// ---- prov 任务队列项 ----
enum {
    PQ_RX = 1,        // 手机写入的一帧;a=len,data=帧字节
    PQ_CONNECT,       // a=conn_handle
    PQ_DISCONNECT,    // a=conn_handle
    PQ_SUBSCRIBE,     // a=1 使能通知 / 0 关闭
    PQ_MTU,           // a=conn_handle,b=协商后 MTU
    PQ_STOP,          // 配网页退出,任务收尾自删
};

typedef struct {
    uint8_t type;
    uint16_t a;
    uint16_t b;       // PQ_MTU:b=协商的 MTU 值(a=连接句柄)
    uint32_t gen;     // 入队时的链路代次(旧连接的迟到事件据此丢弃)
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
    // BLE+Wi-Fi 共享同一射频;默认 ~30ms 密广播会挤占 Wi-Fi 时隙,
    // TLS 握手期间丢包致连接被重置(-0x004C)。放宽到 1.024s,
    // 手机 Web Bluetooth 扫描发现 ~1s,可接受。
    params.itvl_min = 1638;  // 0.625ms 单位 → 1.024s
    params.itvl_max = 1638;
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
        // NimBLE 地址按小端存储:addr[0] 是 MAC 末字节。按常规顺序
        // 取末两字节显示(BLE MAC 98:c3:77:f4:ab:16 → "AB16")。
        snprintf(s_device_id, sizeof(s_device_id), "%02X%02X",
                 addr[1], addr[0]);
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
    // 不在配网会话:什么都不做。控制器常开、无广播无连接,BT 在事件间
    // modem sleep,不与 Wi-Fi 抢射频(见文件头说明)。
}

static void on_reset(int reason)
{
    // 异常复位(非预期):host 会以 100ms 周期重试 sync,on_sync 恢复后
    // 若正处于配网会话会重新开广播。连接表此时已被清空且不会再派发
    // 断开事件,因此在同一串行化边界内作废全部会话身份,避免复用句柄
    // 的新连接被当成旧会话。
    link_on_reset();
    s_hs_synced = false;
    ESP_LOGD(TAG, "NimBLE host 复位: %d", reason);
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
    (void)conn_handle;
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
        item.gen = link_gen_now();
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

// 事件入队:非阻塞。队列满时事件可以丢弃(链路状态与代次已在回调中
// 更新),prov 任务下一轮循环按最新链路对齐会话并恢复,不会继续使用
// 旧授权;丢弃只影响一次唤醒时机,不影响正确性。
static void gap_enqueue(const pq_item_t *item)
{
    if (!s_pq) return;
    if (xQueueSend(s_pq, item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "配网事件队列满,丢弃事件类型 %u",
                 (unsigned)item->type);
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
            item.gen = link_on_connected(event->connect.conn_handle);
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
        item.gen = link_on_disconnected(item.a);
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
            item.gen = link_gen_now();
            link_on_subscribed(event->subscribe.conn_handle,
                               event->subscribe.cur_notify != 0);
            enqueue = true;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %d", event->mtu.value);
        item.type = PQ_MTU;
        item.a = event->mtu.conn_handle;
        item.b = event->mtu.value;
        item.gen = link_gen_now();
        link_on_mtu(event->mtu.conn_handle, event->mtu.value);
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

    if (enqueue) gap_enqueue(&item);
    return 0;
}

// ---------------- 一次性初始化 ----------------

esp_err_t suite_prov_ble_init(void)
{
    if (s_inited) return ESP_OK;

    // 发送串行化边界先于 host 任务启动创建:GAP 回调与 prov 任务都要用。
    if (!s_link_lock) {
        s_link_lock = xSemaphoreCreateMutex();
        if (!s_link_lock) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        // 失败时输出内部堆余量,便于定位 BT 传输池分配失败的原因。
        ESP_LOGE(TAG, "nimble_port_init 失败: %s (空闲堆 %lu, 最大连续内部块 %lu)",
                 esp_err_to_name(err),
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    ESP_LOGI(TAG, "NimBLE 配网服务就绪,空闲堆 %lu,最大连续内部块 %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
// 必需数据组装失败时返回 NULL 并释放整棵树:调用方按"数据构造失败"
// 处理(status 应答回 no_mem、push_status 丢弃),不会发出半份状态。
// sig 在组装 JSON 之前填好,失败时仍与本次系统状态一致,调用方的
// 变化比较不会因成功/失败取值时点不同而抖动。
static cJSON *build_status_data(status_sig_t *sig)
{
    suite_conn_state_t st = suite_connection_state();
    const char *ws = wifi_state_name(st);

    // SSID 掩码:仅保留前两个字符,其余以 *** 代替,不回传完整网络名。
    suite_wifi_cred_t cred;
    char mask[8] = {0};
    bool has_cred = suite_storage_wifi_get(&cred) == ESP_OK;
    if (has_cred) {
        size_t n = strlen(cred.ssid);
        if (n <= 2) {
            strlcpy(mask, "***", sizeof(mask));
        } else {
            snprintf(mask, sizeof(mask), "%c%c***", cred.ssid[0],
                     cred.ssid[1]);
        }
    }

    char ip[16] = {0};
    bool has_ip = suite_connection_ip_str(ip, sizeof(ip));

    int rssi = 0;
    bool has_rssi = suite_connection_ap_rssi(&rssi);

    bool failed = strcmp(ws, "fail") == 0;
    uint16_t reason = suite_connection_last_reason();

    const suite_time_tracker_t *tt = suite_connection_time();
    bool synced = suite_time_is_trusted(tt, suite_connection_now());

    suite_city_t city;
    suite_storage_city_get(&city);

    if (sig) {
        memset(sig, 0, sizeof(*sig));
        sig->wifi = (uint8_t)st;
        sig->reason = reason;
        sig->has_ip = has_ip;
        strlcpy(sig->ip, ip, sizeof(sig->ip));
        sig->rssi_bucket = (int8_t)(rssi / 5);
        sig->time_synced = synced ? 1 : 0;
        strlcpy(sig->city_name, city.name, sizeof(sig->city_name));
        sig->city_la = city.lat_x1e4;
        sig->city_lo = city.lon_x1e4;
        strlcpy(sig->ssid_mask, mask, sizeof(sig->ssid_mask));
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *cj = NULL;
    // 任一次添加失败都放弃整个数据对象:缺字段的状态会被网页当成真实
    // 状态渲染。cJSON 的 Add*ToObject 失败时自己释放它新建的子项,
    // 只有城市子对象需要在这里释放(挂载失败时 cJSON 不接管它)。
    bool built = data != NULL &&
                 cJSON_AddStringToObject(data, "wifi", ws) != NULL;
    if (built && has_cred) {
        built = cJSON_AddStringToObject(data, "ssid", mask) != NULL;
    }
    if (built && has_ip) {
        built = cJSON_AddStringToObject(data, "ip", ip) != NULL;
    }
    if (built && has_rssi) {
        built = cJSON_AddNumberToObject(data, "rssi", rssi) != NULL;
    }
    if (built && failed) {
        built = cJSON_AddNumberToObject(data, "reason", reason) != NULL;
    }
    if (built) {
        built = cJSON_AddBoolToObject(data, "time_synced", synced) != NULL;
    }
    if (built) {
        cj = cJSON_CreateObject();
        built = cj != NULL &&
                cJSON_AddStringToObject(cj, "n", city.name) != NULL &&
                cJSON_AddNumberToObject(cj, "la", city.lat_x1e4) != NULL &&
                cJSON_AddNumberToObject(cj, "lo", city.lon_x1e4) != NULL &&
                cJSON_AddItemToObject(data, "city", cj);
        if (!built) cJSON_Delete(cj);  // 未挂载(或创建失败):归调用方释放
    }
    if (!built) {
        cJSON_Delete(data);
        return NULL;
    }
    return data;
}

// prov 任务的会话视图:纯逻辑会话(授权/订阅/MTU)加上它所属的链路代次。
// 代次是连接身份:句柄会复用,只有代次能区分"旧连接断开"与"同句柄新连接"。
typedef struct {
    suite_prov_session_t sess;
    uint32_t gen;
} prov_session_t;

// 会话是否仍属于当前链路(连接存在且代次未被断连/换机推进)。
static bool prov_session_current(const prov_session_t *ps)
{
    prov_link_t link = link_snapshot();
    return ps->gen == link.gen && ps->sess.conn != SUITE_PROV_CONN_NONE &&
           ps->sess.conn == link.conn;
}

// 把会话对齐到最新链路快照:
//   - 代次变化(断连/换机)⇒ 授权、订阅、MTU、半条消息全部立即作废,
//     新连接必须重新 hello;队列满丢事件也由这里的代次比较恢复;
//   - 订阅与 MTU 是当前链路的属性,每个循环都采用,因此即使对应事件
//     被队列丢弃(如阻塞扫描期间的突发)也不会丢失。
static void session_sync_link(prov_session_t *ps, suite_prov_reasm_t *reasm,
                              uint8_t *buf, size_t cap, bool *force_status)
{
    prov_link_t link = link_snapshot();
    if (link.gen != ps->gen) {
        uint32_t old_gen = ps->gen;
        suite_prov_session_connect(&ps->sess, link.conn, s_expected_pin);
        ps->gen = link.gen;
        suite_prov_reasm_init(reasm, buf, cap);
        *force_status = true;
        ESP_LOGI(TAG, "配网链路代次更新 %lu → %lu,旧会话已失效",
                 (unsigned long)old_gen, (unsigned long)link.gen);
    }
    suite_prov_session_subscribe(&ps->sess, link.subscribed);
    if (link.conn != SUITE_PROV_CONN_NONE) {
        (void)suite_prov_session_set_mtu(&ps->sess, link.conn, link.mtu);
    }
}

// 提交一帧:最终代次校验与通知提交必须处于同一串行化边界。
// 单独再加一次检查并不能消除窗口(校验与提交之间 GAP 回调仍可推进代次
// 并让句柄指向新连接),因此这里持发送串行化边界完成"校验 + 提交":
//   - 持锁期间代次不会变化,句柄仍属于会话所属的连接;
//   - 旧连接已删除时按句柄找不到连接,协议栈释放 mbuf 并返回错误,
//     不会把帧交给尚未 hello 的新连接。
// 返回 false 表示该帧未提交(调用方应停止本次应答的剩余帧)。
static bool tx_commit_frame(const prov_session_t *ps, struct os_mbuf *om)
{
    bool current;
    int rc = 0;
    link_lock_take();
    current = prov_session_current(ps);
    if (current) {
        rc = ble_gatts_notify_custom(ps->sess.conn, s_tx_val_handle, om);
    }
    link_lock_give();
    if (!current) {
        // 校验不通过:协议栈尚未接管 mbuf,必须自行释放。
        os_mbuf_free_chain(om);
        ESP_LOGD(TAG, "链路已切换,丢弃待提交的通知帧");
        return false;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 发送失败: %d", rc);
        return false;
    }
    return true;
}

static void tx_json(const prov_session_t *ps, const char *text)
{
    if (!ps->sess.subscribed ||
        ps->sess.conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    size_t payload = (size_t)ps->sess.mtu - SUITE_PROV_HEADER_LEN - 3 /* ATT 头 */;
    if (payload < SUITE_PROV_PAYLOAD_MIN) payload = SUITE_PROV_PAYLOAD_MIN;
    if (payload > SUITE_PROV_PAYLOAD_MAX) payload = SUITE_PROV_PAYLOAD_MAX;

    size_t total = strlen(text);
    size_t frames = suite_prov_frame_count(total, payload);
    uint8_t buf[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];
    for (size_t i = 0; i < frames; i++) {
        // 每帧构造与提交前后都校验代次:覆盖阻塞扫描返回、逐帧发送中、
        // 校验通过到提交通知之间的断连/换机——旧会话立即停止发送。
        if (!prov_session_current(ps)) {
            ESP_LOGD(TAG, "链路已切换,放弃会话通知");
            return;
        }
        size_t flen = suite_prov_frame_build((const uint8_t *)text, total,
                                             payload, i, buf, sizeof(buf));
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)flen);
        if (!om) {
            ESP_LOGW(TAG, "notify mbuf 分配失败,丢弃剩余帧");
            return;
        }
        if (!tx_commit_frame(ps, om)) {
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

// 失败路径要在成功应答构造函数里回错误帧,这里前向声明(错误应答自身
// 不调用任何其它应答构造,不存在递归)。
static void reply_err(const prov_session_t *ps, double id, const char *err);

// 序列化缓冲由 cJSON 分配(global_hooks;固件为默认 malloc,主机测试会
// 换成计数/注入钩子),必须用 cJSON_free 归还:不能假定 libc free 与
// cJSON 的分配器一致。
static void s_print_free(char *text)
{
    cJSON_free(text);
}

// 成功应答。data 的所有权自调用起归本函数:
//   - root 与 id/ok/data 全部挂载成功 ⇒ data 随 root 一起释放;
//   - root 创建失败或任一字段挂载失败 ⇒ data 未被 root 接管,由本函数释放。
// cJSON 的 AddItemToObject 只在成功时接管 item:cJSON 1.7.19 的
// add_item_to_object 在键复制失败或前置条件不满足时直接返回 false,
// item 仍归调用方,遗漏释放即泄漏整棵结果子树。
// 任一步失败都不发送残缺/虚假的成功帧:成功帧缺 data 时网页会把
// resolve(undefined) 当成结果(例如扫描"没有网络"),缺 id 时整帧被丢弃、
// 手机只能等超时。失败时改发完整 no_mem 错误应答;错误应答自身也无法
// 完整构造/发送时静默丢弃(tx_json 保证不落半帧),不做递归重试。
// 契约说明:本文件的成功命令(wifi_scan/status/provinces/cities/hello/
// wifi_set/city_set)都要求 data 字段,无内容的命令返回 {};因此 data 为
// NULL 一律视为"数据构造失败",不得当作"合法无 data 应答"。将来若出现
// 契约上不带 data 的成功应答,应另加显式的无 data 构造函数,而不是放宽
// 本判据,避免把两种情形混为一谈。
static void reply_ok(const prov_session_t *ps, double id, cJSON *data)
{
    cJSON *root = (data != NULL) ? cJSON_CreateObject() : NULL;
    bool attached = root != NULL &&
                    cJSON_AddNumberToObject(root, "id", id) != NULL &&
                    cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                    cJSON_AddItemToObject(root, "data", data);

    if (attached) {
        char *text = s_print(root);
        if (text != NULL) {
            tx_json(ps, text);
            s_print_free(text);
            cJSON_Delete(root);  // data 已挂载,随 root 释放
            return;
        }
        // 序列化失败:成功帧发不出去,按失败处理,不发送半份成功。
    }

    cJSON_Delete(root);                 // 释放已挂载的 id/ok(attached 时含 data)
    if (!attached) cJSON_Delete(data);  // 未挂载:所有权仍在本函数
    reply_err(ps, id, "no_mem");
}

// 错误应答。id/ok/err 三个字段缺一不可:缺字段的错误帧会被网页当成
// 未知错误或直接丢弃。构造或序列化失败时放弃发送并释放全部资源;
// 这里不调用其它应答构造函数,避免错误处理路径再次失败或递归。
static void reply_err(const prov_session_t *ps, double id, const char *err)
{
    cJSON *root = cJSON_CreateObject();
    bool complete = root != NULL &&
                    cJSON_AddNumberToObject(root, "id", id) != NULL &&
                    cJSON_AddBoolToObject(root, "ok", false) != NULL &&
                    cJSON_AddStringToObject(root, "err", err) != NULL;
    if (complete) {
        char *text = s_print(root);
        if (text != NULL) {
            tx_json(ps, text);
            s_print_free(text);
        }
    }
    cJSON_Delete(root);
}

// 状态事件推送(ev=status)。data 所有权同 reply_ok:挂载成功随 root
// 释放,失败由本函数释放。两者任一构造失败都静默丢弃:状态事件没有 id,
// 无法回错误帧,半份 {"ev":"status"} 会被网页渲染成"空闲"等错误状态。
static void push_status(const prov_session_t *ps)
{
    status_sig_t sig;
    cJSON *data = build_status_data(&sig);
    cJSON *root = (data != NULL) ? cJSON_CreateObject() : NULL;
    bool attached = root != NULL &&
                    cJSON_AddStringToObject(root, "ev", "status") != NULL &&
                    cJSON_AddItemToObject(root, "data", data);
    if (attached) {
        char *text = s_print(root);
        if (text != NULL) {
            tx_json(ps, text);
            s_print_free(text);
        }
    }
    cJSON_Delete(root);
    if (!attached) cJSON_Delete(data);
}

// ---------------- 命令处理 ----------------

static void cmd_wifi_scan(double id, prov_session_t *ps)
{
    wifi_scan_config_t scfg = {0};
    scfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scfg.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 扫描启动失败: %s", esp_err_to_name(err));
        reply_err(ps, id, "scan_busy");
        return;
    }

    // 扫描成功后,扫描结果由 Wi-Fi 驱动动态持有(ESP-IDF 5.5.3):
    // 每条退出路径都必须消费(esp_wifi_scan_get_ap_records,驱动随之
    // 释放全部内存)或丢弃(esp_wifi_clear_ap_list),否则泄漏驱动内存。

    // 阻塞扫描(秒级)期间可能已断连/换机,甚至句柄已被新手机复用:
    // 结果不再属于任何会话,直接释放,不做后续解析与发送。
    if (!prov_session_current(ps)) {
        (void)esp_wifi_clear_ap_list();
        ESP_LOGD(TAG, "扫描期间链路切换,丢弃扫描结果");
        return;
    }

    uint16_t n = SCAN_MAX_HINTS;
    wifi_ap_record_t *aps = calloc(n, sizeof(wifi_ap_record_t));
    if (!aps) {
        // 应用侧分配失败:驱动仍持有结果,必须由本函数释放。
        (void)esp_wifi_clear_ap_list();
        reply_err(ps, id, "no_mem");
        return;
    }
    err = esp_wifi_scan_get_ap_records(&n, aps);
    if (err != ESP_OK) {
        // 获取失败:不能把失败伪装成成功(不发 nets 应答);结果通常
        // 仍由驱动持有,按 ESP-IDF 语义用 clear 兜底释放,避免泄漏。
        ESP_LOGW(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
        (void)esp_wifi_clear_ap_list();
        free(aps);
        reply_err(ps, id, "scan_failed");
        return;
    }

    // 同名去重取最强,用独立堆表避免大数组上栈。
    typedef struct {
        char ssid[33];
        int8_t rssi;
        uint8_t sec;
        bool used;
    } scan_ent_t;
    // 去重表只在有结果时分配:calloc(0) 在部分堆实现上返回 NULL,零 AP
    // 不是内存不足,仍要成功返回 data.nets=[]。
    scan_ent_t *ents = (n > 0) ? calloc(n, sizeof(scan_ent_t)) : NULL;
    cJSON *nets = cJSON_CreateArray();

    // 组装应答数据。任何应用侧分配或 JSON 组装失败都必须放弃整个应答并
    // 走错误路径,不能把部分结果、缺字段结果或"没有网络"伪装成成功。
    bool built = (nets != NULL) && (n == 0 || ents != NULL);
    if (built) {
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
            // 添加失败时 cJSON 不接管 net/cJSON 子项的所有权:
            // cJSON_Add*ToObject 失败由函数内部释放自己的子项并返回
            // NULL;cJSON_AddItemToArray 失败时 net 仍归调用方。
            cJSON *net = cJSON_CreateObject();
            if (!net ||
                !cJSON_AddStringToObject(net, "ssid", ents[i].ssid) ||
                !cJSON_AddNumberToObject(net, "rssi", ents[i].rssi) ||
                // sec 直接使用 WIFI_AUTH_* 枚举序号,网页据此显示锁标。
                !cJSON_AddNumberToObject(net, "sec", ents[i].sec)) {
                cJSON_Delete(net);
                built = false;
                break;
            }
            if (!cJSON_AddItemToArray(nets, net)) {
                cJSON_Delete(net);
                built = false;
                break;
            }
        }
    }
    free(aps);
    free(ents);

    if (!built) {
        // 会话仍有效且错误帧能分配时会发送 no_mem;若错误帧也无法组装/
        // 发送,tx_json 会静默丢弃——无论如何都不会出现 ok:true 的半份
        // 扫描结果。aps/ents/驱动结果已在上面释放,没有泄漏。
        cJSON_Delete(nets);
        reply_err(ps, id, "no_mem");
        return;
    }
    // 应答契约(与 provisioning/web/app.js 和 README 一致):
    // {"nets":[{"ssid":...,"rssi":...,"sec":...}, ...]}。
    // 不能把数组直接作为 data 返回:网页读取 data.nets,否则扫描结果
    // 永远被当作空列表。
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(nets);
        reply_err(ps, id, "no_mem");
        return;
    }
    if (!cJSON_AddItemToObject(data, "nets", nets)) {
        // 键分配失败时 nets 未被 data 接管,两者都要释放。
        cJSON_Delete(nets);
        cJSON_Delete(data);
        reply_err(ps, id, "no_mem");
        return;
    }
    reply_ok(ps, id, data);
}

static void cmd_provinces(double id, prov_session_t *ps)
{
    // 任一条目组装失败都放弃整个应答:少条目的列表会让网页把省级索引
    // 对错位置,不能当作成功结果发送。cJSON 的 Add*ToObject 失败时自己
    // 释放新建的子项,但 AddItemToArray 失败不接管未加入的条目。
    cJSON *arr = cJSON_CreateArray();
    bool built = (arr != NULL);
    int n = suite_city_province_count();
    for (int i = 0; built && i < n; i++) {
        const suite_province_def_t *p = suite_city_province_at(i);
        cJSON *o = cJSON_CreateObject();
        if (!o || !cJSON_AddNumberToObject(o, "i", i) ||
            !cJSON_AddStringToObject(o, "n", p->name) ||
            !cJSON_AddItemToArray(arr, o)) {
            cJSON_Delete(o);
            built = false;
        }
    }
    if (!built) {
        cJSON_Delete(arr);
        reply_err(ps, id, "no_mem");
        return;
    }
    reply_ok(ps, id, arr);
}

static void cmd_cities(const cJSON *root, double id, prov_session_t *ps)
{
    cJSON *jp = cJSON_GetObjectItem(root, "p");
    if (!cJSON_IsNumber(jp)) {
        reply_err(ps, id, "bad_arg");
        return;
    }
    int p = jp->valueint;
    const suite_province_def_t *prov = suite_city_province_at(p);
    if (!prov) {
        reply_err(ps, id, "bad_arg");
        return;
    }
    // 同 cmd_provinces:部分城市列表会让网页选到错误的坐标。
    cJSON *arr = cJSON_CreateArray();
    bool built = (arr != NULL);
    for (int i = 0; built && i < prov->city_count; i++) {
        const suite_city_def_t *c = &prov->cities[i];
        cJSON *o = cJSON_CreateObject();
        if (!o || !cJSON_AddNumberToObject(o, "i", i) ||
            !cJSON_AddStringToObject(o, "n", c->name) ||
            !cJSON_AddNumberToObject(o, "la", c->lat_x1e4) ||
            !cJSON_AddNumberToObject(o, "lo", c->lon_x1e4) ||
            !cJSON_AddItemToArray(arr, o)) {
            cJSON_Delete(o);
            built = false;
        }
    }
    if (!built) {
        cJSON_Delete(arr);
        reply_err(ps, id, "no_mem");
        return;
    }
    reply_ok(ps, id, arr);
}

static void handle_message(const uint8_t *msg, size_t len,
                           prov_session_t *ps, bool *force_status)
{
    cJSON *root = cJSON_ParseWithLength((const char *)msg, len);
    if (!root) {
        reply_err(ps, 0, "bad_json");
        return;
    }

    cJSON *jid = cJSON_GetObjectItem(root, "id");
    double id = cJSON_IsNumber(jid) ? jid->valuedouble : 0;
    cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(jcmd)) {
        reply_err(ps, id, "bad_arg");
        cJSON_Delete(root);
        return;
    }
    const char *cmd = jcmd->valuestring;

    // 报文已属于旧会话(解析期间链路被断开/更换):静默丢弃,
    // 不产生任何应答或副作用。
    if (!prov_session_current(ps)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd, "hello") == 0) {
        cJSON *jpin = cJSON_GetObjectItem(root, "pin");
        if (!cJSON_IsString(jpin)) {
            reply_err(ps, id, "bad_pin");
            cJSON_Delete(root);
            return;
        }
        suite_prov_pin_result_t pr =
            suite_prov_pin_verify(&ps->sess.pin, jpin->valuestring,
                                  strlen(jpin->valuestring));
        if (pr == SUITE_PROV_PIN_OK) {
            s_phase = SUITE_PROV_PHASE_AUTHED;
            reply_ok(ps, id, cJSON_CreateObject());
            *force_status = true;
        } else {
            reply_err(ps, id, "bad_pin");
            if (pr == SUITE_PROV_PIN_LOCKED && prov_session_current(ps)) {
                ESP_LOGW(TAG, "配对码连续错误,断开手机");
                // 配对码不得出现在日志;仅记录断开动作。锁定只断开本
                // 会话的连接,不误伤可能已复用句柄的新连接。
                (void)ble_gap_terminate(ps->sess.conn,
                                        BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        cJSON_Delete(root);
        return;
    }

    // 除 hello 外的命令都必须先通过配对码授权。
    if (!suite_prov_session_authorized(&ps->sess)) {
        reply_err(ps, id, "need_pin");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        reply_ok(ps, id, build_status_data(NULL));
    } else if (strcmp(cmd, "wifi_scan") == 0) {
        cmd_wifi_scan(id, ps);
    } else if (strcmp(cmd, "provinces") == 0) {
        cmd_provinces(id, ps);
    } else if (strcmp(cmd, "cities") == 0) {
        cmd_cities(root, id, ps);
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
            reply_err(ps, id, "bad_arg");
        } else if (!prov_session_current(ps)) {
            // 校验参数期间链路已断开/更换:不写 NVS、不应答。
        } else {
            suite_wifi_cred_t cred = {0};
            strlcpy(cred.ssid, jssid->valuestring, sizeof(cred.ssid));
            strlcpy(cred.password, jpass->valuestring, sizeof(cred.password));
            esp_err_t err = suite_storage_wifi_set(&cred);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi 凭据写 NVS 失败: %s",
                         esp_err_to_name(err));
                reply_err(ps, id, "storage");
            } else {
                err = suite_connection_apply_new_wifi();
                if (err != ESP_OK) {
                    reply_err(ps, id, "conn_busy");
                } else {
                    ESP_LOGI(TAG, "新 Wi-Fi 凭据已保存并应用(SSID 长度 %d)",
                             (int)strlen(cred.ssid));  // 不打印 SSID/密码
                    *force_status = true;
                    reply_ok(ps, id, cJSON_CreateObject());
                }
            }
        }
    } else if (strcmp(cmd, "city_set") == 0) {
        cJSON *jp = cJSON_GetObjectItem(root, "p");
        cJSON *jc = cJSON_GetObjectItem(root, "c");
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jc)) {
            reply_err(ps, id, "bad_arg");
        } else {
            const suite_province_def_t *prov =
                suite_city_province_at(jp->valueint);
            int ci = jc->valueint;
            if (!prov || ci < 0 || ci >= prov->city_count) {
                reply_err(ps, id, "bad_arg");
            } else if (!prov_session_current(ps)) {
                // 校验期间链路已断开/更换:不写 NVS、不应答。
            } else {
                const suite_city_def_t *d = &prov->cities[ci];
                suite_city_t city = {0};
                strlcpy(city.name, d->name, sizeof(city.name));
                city.lat_x1e4 = d->lat_x1e4;
                city.lon_x1e4 = d->lon_x1e4;
                city.tz_off_s = 8 * 3600;
                esp_err_t err = suite_storage_city_set(&city);
                if (err != ESP_OK) {
                    reply_err(ps, id, "storage");
                } else {
                    (void)suite_weather_request_refresh();
                    *force_status = true;
                    reply_ok(ps, id, cJSON_CreateObject());
                }
            }
        }
    } else {
        reply_err(ps, id, "bad_cmd");
    }

    cJSON_Delete(root);
}

// ---------------- prov 任务主体 ----------------

static void prov_task(void *arg)
{
    (void)arg;
    // 会话随链路重建:连接建立时重新开始配对码授权,断开/换机后旧授权
    // 立即失效;代次隔离同句柄复用,不继承上一连接的任何状态。
    prov_session_t ps;
    suite_prov_session_init(&ps.sess);
    ps.gen = 0;  // 全局初始代次;首轮对齐处理启动前已存在的链路状态

    static uint8_t msg_buf[SUITE_PROV_MSG_MAX];  // 静态,避开任务栈
    suite_prov_reasm_t reasm;
    suite_prov_reasm_init(&reasm, msg_buf, sizeof(msg_buf));

    bool force_status = true;
    status_sig_t last_sig;
    memset(&last_sig, 0, sizeof(last_sig));

    // CONFIG_FREERTOS_HZ=1000,故 tick 计数即毫秒数。
    TickType_t last_status = xTaskGetTickCount();

    for (;;) {
        pq_item_t item;
        bool got = xQueueReceive(s_pq, &item, pdMS_TO_TICKS(200));

        // 每轮先把会话对齐到最新链路快照(在处理事件之前):断连/换机
        // 立即作废旧授权与半条消息;事件队列满也不影响该恢复路径。
        // 事件本身只是唤醒手段与入站帧载体,不再直接改写会话状态。
        session_sync_link(&ps, &reasm, msg_buf, sizeof(msg_buf), &force_status);

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

            case PQ_RX:
                // 只接受当前链路的帧:旧连接断开/句柄被复用后,积压的
                // RX 一律丢弃,不会进入新会话的重组缓冲。
                if (item.gen == ps.gen) {
                    const uint8_t *out = NULL;
                    size_t out_len = 0;
                    int r = suite_prov_reasm_feed(
                        &reasm, item.data, item.a,
                        (uint32_t)xTaskGetTickCount(), &out, &out_len);
                    if (r > 0) {
                        handle_message(out, out_len, &ps, &force_status);
                    } else if (r < 0) {
                        // 非法帧:静默丢弃半条消息,等下一个 START 重开。
                        ESP_LOGD(TAG, "收到非法配网帧,重组会话已重置");
                    }
                }
                break;

            case PQ_CONNECT:
            case PQ_DISCONNECT:
            case PQ_SUBSCRIBE:
            case PQ_MTU:
                // 链路状态(连接句柄/订阅/MTU/代次)已由 GAP 回调写入
                // 快照,上面的对齐已按最新状态处理;事件仅用于唤醒。
                break;

            default:
                break;
            }
        }

        // 半条消息 3s 超时清理。
        (void)suite_prov_reasm_tick(&reasm, (uint32_t)xTaskGetTickCount());

        // 每 500ms 检测一次状态变化,变化时主动 notify(连接 GOT_IP、
        // 断开带 reason、SNTP 同步、城市保存等都经此通道触达手机)。
        // 状态含城市坐标、SSID 掩码、本地 IP:必须与命令同一授权条件,
        // 未通过配对码的连接不得收到(每次连接都要重新 hello)。
        TickType_t now = xTaskGetTickCount();
        if (suite_prov_session_status_allowed(&ps.sess) &&
            (force_status || now - last_status >= pdMS_TO_TICKS(500))) {
            last_status = now;
            status_sig_t sig;
            cJSON *tmp = build_status_data(&sig);
            cJSON_Delete(tmp);
            if (force_status || memcmp(&sig, &last_sig, sizeof(sig)) != 0) {
                push_status(&ps);
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
