#pragma once

// tests/ble_stubs/prov_test_stubs.h —— suite_prov_ble.c 主机测试的最小
// ESP-IDF / FreeRTOS / NimBLE 边界。
//
// prov_task 运行在真实线程上(prov_runtime.c):队列接收由测试逐次放行
// ("200ms 超时"或"事件到达");GAP/GATT 回调、扫描阻塞期间的链路切换、
// 多帧通知出口(逐帧观察与注入)都由测试驱动;换机/reset 与发送的交错
// 由独立 host 线程 + 通知提交闸门构造(见下方闸门 API)。断言对象是
// 行为:队列满与阻塞扫描后的会话隔离、同句柄复用的代次隔离、MTU 与
// 订阅不跨连接继承、未认证连接收到零帧受保护数据、正常配对/错误配对码/
// 状态推送流程。
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- esp_err ----
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
const char *esp_err_to_name(esp_err_t error);

void test_log(const char *tag, const char *format, ...);
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGD(...) test_log(__VA_ARGS__)

// ---- FreeRTOS 子集 ----
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef struct test_queue *QueueHandle_t;
typedef struct test_task *TaskHandle_t;
typedef struct test_sem *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_port_enter(portMUX_TYPE *mux);
void test_port_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) test_port_enter(mux)
#define portEXIT_CRITICAL(mux) test_port_exit(mux)
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)

QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
// 互斥量(生产的发送串行化边界):FreeRTOS xSemaphoreCreateMutex 语义,
// 普通非递归互斥量;同线程重入在桩内直接断言失败。
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
void vSemaphoreDelete(SemaphoreHandle_t sem);

// ---- 堆诊断 ----
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t esp_get_free_heap_size(void);

// ---- esp_app_desc ----
typedef struct {
    char version[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);

// ---- esp_wifi(扫描子集) ----
typedef struct {
    int scan_type;
    bool show_hidden;
} wifi_scan_config_t;
typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    int authmode;
} wifi_ap_record_t;
#define WIFI_SCAN_TYPE_ACTIVE 0
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number,
                                       wifi_ap_record_t *ap_records);
// 丢弃驱动仍持有的扫描结果(ESP-IDF 5.5.3:释放扫描期分配的 AP 列表)。
esp_err_t esp_wifi_clear_ap_list(void);

// ---- NimBLE 子集(生产只用到的部分) ----
typedef struct {
    uint8_t padding;
} ble_uuid_t;
typedef struct {
    ble_uuid_t u;
    uint8_t value[16];
} ble_uuid128_t;
#define BLE_UUID128_INIT(...)                                                  \
    {                                                                          \
        .u = { 0 }, .value = { __VA_ARGS__ }                                   \
    }

// GAP 事件(字段与生产访问一致;测试直接构造事件驱动回调)。
struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct {
            int reason;
            struct {
                uint16_t conn_handle;
            } conn;
        } disconnect;
        struct {
            uint16_t conn_handle;
            uint16_t attr_handle;
            uint8_t reason;
            uint8_t cur_notify;
        } subscribe;
        struct {
            uint16_t conn_handle;
            uint16_t channel_id;
            uint16_t value;
        } mtu;
        struct {
            int reason;
        } adv_complete;
    };
};
#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_ADV_COMPLETE 2
#define BLE_GAP_EVENT_SUBSCRIBE 14
#define BLE_GAP_EVENT_MTU 15

struct ble_hs_adv_fields {
    uint8_t flags;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    uint8_t uuids128_is_complete;
    const uint8_t *name;
    uint8_t name_len;
    uint8_t name_is_complete;
};
struct ble_gap_adv_params {
    uint8_t conn_mode;
    uint8_t disc_mode;
    uint16_t itvl_min;
    uint16_t itvl_max;
};

#define BLE_HS_ADV_F_DISC_GEN 0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
#define BLE_GAP_CONN_MODE_UND 0
#define BLE_GAP_DISC_MODE_GEN 0
#define BLE_HS_FOREVER INT32_MAX
#define BLE_HS_CONN_HANDLE_NONE 0xFFFFu
#define BLE_HS_EALREADY 5
#define BLE_HS_ETIMEOUT 13
#define BLE_ERR_REM_USER_CONN_TERM 19

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *fields);
int ble_gap_adv_start(uint8_t own_addr_type, const void *direct_addr,
                      int32_t duration_ms, const struct ble_gap_adv_params *params,
                      int (*cb)(struct ble_gap_event *event, void *arg),
                      void *cb_arg);
int ble_gap_adv_stop(void);
int ble_gap_terminate(uint16_t conn_handle, int reason);
int ble_hs_util_ensure_addr(int prefer_random);
int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type);
int ble_hs_id_copy_addr(uint8_t addr_type, uint8_t *out_addr,
                        int *out_addr_len);

struct ble_hs_cfg_t {
    void (*sync_cb)(void);
    void (*reset_cb)(int reason);
};
extern struct ble_hs_cfg_t ble_hs_cfg;

// GATT 服务表(生产静态定义;桩不解析,只允许注册)。
struct ble_gatt_access_ctxt;
typedef int (*ble_gatt_access_fn)(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn access_cb;
    uint8_t flags;
    uint16_t *val_handle;
};
struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    struct ble_gatt_chr_def *characteristics;
};
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_WRITE 0x01
#define BLE_GATT_CHR_F_WRITE_NO_RSP 0x02
#define BLE_GATT_CHR_F_READ 0x04
#define BLE_GATT_CHR_F_NOTIFY 0x08
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_UNLIKELY 0x0E
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0D
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11

int ble_att_set_preferred_mtu(uint16_t mtu);
int ble_svc_gap_init(void);
int ble_svc_gatt_init(void);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *svcs);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *svcs);
int ble_svc_gap_device_name_set(const char *name);
int ble_uuid_cmp(const ble_uuid_t *uuid1, const ble_uuid_t *uuid2);

esp_err_t nimble_port_init(void);
void nimble_port_run(void);
void nimble_port_freertos_init(TaskFunction_t host_fn);
void nimble_port_freertos_deinit(void);

// ---- mbuf ----
#define SUITE_PROV_MBUF_DATA 244
struct os_mbuf {
    uint16_t len;
    uint8_t data[SUITE_PROV_MBUF_DATA];
};
#define OS_MBUF_PKTLEN(om) ((om)->len)
int os_mbuf_append(struct os_mbuf *m, const void *data, uint16_t len);
void os_mbuf_free_chain(struct os_mbuf *om);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len,
                        uint16_t *out_copy_len);

struct ble_gatt_access_ctxt {
    int op;
    const struct ble_gatt_chr_def *chr;
    struct os_mbuf *om;
};

int ble_gatts_notify_custom(uint16_t conn_handle, uint16_t att_handle,
                            struct os_mbuf *om);

// ---- 测试控制面 ----

// 等待 prov 任务线程停靠在下一次队列接收或结束;返回本次请求的等待时长
// (ms);任务结束返回 0。
unsigned long test_prov_wait_park(void);
// 队列为空断言后按"200ms 超时"恢复执行。
void test_prov_resume_timeout(void);
// 队列非空断言后按"事件到达"恢复执行(一次恢复处理完队列中全部事件)。
void test_prov_resume_event(void);
bool test_prov_finished(void);
// 队列发送唤醒阻塞中的接收者(真实 FreeRTOS 语义)。默认关闭以保持
// 逐次放行的确定性,仅配网页退出(suite_prov_ble_end)等需要真实唤醒
// 的路径开启。
void test_prov_set_auto_wake(bool on);

// 阻塞扫描钩子:esp_wifi_scan_start 返回前在 prov 任务线程内调用一次
// (模拟 host 任务在扫描期间的链路事件);设置后自动解除。
void test_prov_set_scan_hook(void (*hook)(void));
// 扫描返回的 AP 列表(供 cmd_wifi_scan 的 esp_wifi_scan_get_ap_records)。
void test_prov_set_scan_aps(const wifi_ap_record_t *aps, unsigned count);

// ---- 扫描结果所有权模型 ----
//
// ESP-IDF 5.5.3:esp_wifi_scan_start() 成功后,驱动动态持有扫描结果;
// 调用方必须用 esp_wifi_scan_get_ap_records() 消费(驱动随之释放全部
// 内存)或 esp_wifi_clear_ap_list() 丢弃。桩按真实所有权建模:
//   - 结果用 malloc 分配,仅由 get/clear 之一释放;
//   - get 失败时结果仍归驱动所有(必须由调用方 clear,否则泄漏);
//   - 结果不存在时 clear 是安全空操作(不重复释放)。
typedef struct {
    unsigned start_calls;   // esp_wifi_scan_start 调用次数
    unsigned start_fail;    // 启动失败次数
    unsigned allocs;        // 扫描成功后的结果分配次数
    unsigned get_calls;     // esp_wifi_scan_get_ap_records 调用次数
    unsigned get_fail;      // 获取失败次数
    unsigned clear_calls;   // esp_wifi_clear_ap_list 调用次数
    unsigned frees;         // 驱动释放次数(消费 + 丢弃)
    unsigned overwritten;   // 上一次结果未释放就被新扫描覆盖(应为 0)
    bool live;              // 驱动当前是否仍持有未释放的结果
} prov_scan_stats_t;

const prov_scan_stats_t *test_prov_scan_stats(void);
// 让下一次扫描启动返回错误(ESP_ERR_WIFI_NOT_STARTED 语义)。
void test_prov_set_scan_start_fail(bool fail);
// 让下一次 esp_wifi_scan_get_ap_records 失败:结果仍由驱动持有。
void test_prov_set_scan_get_fail(bool fail);

// ---- 连接生命周期与提交串行化模型 ----
//
// 真实栈的 ble_gatts_notify_custom() 在提交时按连接表解析句柄:A 断开、
// B 以同一句柄连接后,旧应答会落到 B。生产把"最终代次校验 + 提交"放进
// 自己的生命周期锁(连接/断开/主机复位 GAP 回调推进代次前也取同一把锁)。
// 桩复现:
//   - 连接表按句柄记录"连接序号",attach 返回新序号(句柄复用=新序号);
//   - 提交时按句柄解析:未连接则拒绝并释放 mbuf(与 ble_att_tx 一致);
//   - 测试通过"通知提交闸门 + 独立 host 线程"构造真实交错(见下),
//     生产锁是否把换机挡在提交窗口之外由断言直接判定。
unsigned test_prov_conn_attach(uint16_t conn);
void test_prov_conn_detach(uint16_t conn);
unsigned test_prov_conn_serial(uint16_t conn);
void test_prov_conn_set_mtu(uint16_t conn, unsigned mtu);
unsigned test_prov_conn_mtu(uint16_t conn);

// 发送出口观察者:每次 ble_gatts_notify_custom 提交一帧时调用(句柄、
// 提交时该句柄的连接序号、该连接的 ATT MTU、帧字节)。测试按连接序号
// 断言"未认证连接收到零帧(含不完整片段)",并按 ATT MTU 校验帧长。
void test_prov_set_frame_sink(void (*sink)(uint16_t conn, unsigned conn_serial,
                                           unsigned att_mtu,
                                           const uint8_t *frame, size_t len));
// 提交前钩子:mbuf 分配后、ble_gatts_notify_custom 之前,在任务线程内
// 调用一次(模拟"校验通过到提交通知之间"的链路切换);自动解除。
void test_prov_set_precommit_hook(void (*hook)(void));

// 通知提交闸门(双线程交错控制):武装后,第 skip+1 次
// ble_gatts_notify_custom 在"按连接表解析句柄"之前停住,直到测试放行。
// 发送线程在闸门内仍持有生产的生命周期锁;测试从这个确定点启动独立
// host 线程执行换机/reset,交错完全由测试驱动,桩不做"生产是否持锁"探针。
void test_prov_arm_notify_gate(unsigned skip);
// 等待发送线程进入闸门(超时返回 false;失败即断言,不挂死门禁)。
bool test_prov_wait_notify_gate(unsigned long timeout_ms);
// 放行闸门。
void test_prov_release_notify_gate(void);
// 行为顺序计数:host 线程完成换机序列、发送线程通过闸门各取一个序号。
// 正确串行化下 host 完成序号 > 闸门序号;换机提前生效则相反。
uint32_t test_prov_seq_bump(void);
uint32_t test_prov_gate_seq(void);

// ---- 资源存活计数(队列/信号量/任务/在途 mbuf) ----
unsigned test_prov_live_queues(void);
unsigned test_prov_live_sems(void);
unsigned test_prov_live_tasks(void);
unsigned test_prov_live_mbufs(void);

// 应用侧分配失败注入:从本次注入开始的第 nth 次(1-based)生产源码
// calloc 返回 NULL。测试用 "#define calloc test_prov_calloc" 只拦截
// 生产源码的分配。零元素分配按严格堆语义直接返回 NULL。
void test_prov_fail_calloc_at(unsigned nth);
void *test_prov_calloc(size_t count, size_t size);

// ble_gap_terminate 调用次数(配对码锁定应断开当前连接)。
unsigned test_prov_terminate_calls(void);
