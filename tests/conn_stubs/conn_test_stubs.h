#pragma once

// tests/conn_stubs/conn_test_stubs.h —— suite_connection.c 主机测试的最小
// ESP-IDF / FreeRTOS 边界。
//
// conn_task 运行在真实线程上(conn_runtime.c);队列接收由测试逐次放行
// ("超时"或"事件到达"),网络栈每一步都可注入失败,用于验证:
//   - 初始化失败进入有限等待的可恢复状态(不无限等待启动事件);
//   - 资源恢复后重新初始化并成功联网;
//   - 部分初始化完整回滚,不留错误标志、重复注册或泄漏;
//   - 事件处理器"延迟注销失败"(事件循环锁忙 + 清理事件分配失败,
//     按 ESP-IDF 5.5.3 esp_event 源码建模)后的所有权保留与有界回收;
//   - 正常连接与"应用新凭据"路径不受影响。
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);
#define ESP_ERROR_CHECK(x) do { assert((x) == ESP_OK); } while (0)

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
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)

QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t ticks);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
// 互斥量:真实线程锁,用于 netif 生命周期(publish/read/destroy)。
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

// ---- esp_event ----
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);
#define WIFI_EVENT "wifi"
#define IP_EVENT "ip"
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 0
#define WIFI_EVENT_STA_DISCONNECTED 1
#define WIFI_EVENT_STA_STOP 2
#define WIFI_EVENT_STA_CONNECTED 3
#define IP_EVENT_STA_GOT_IP 0
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t callback,
                                              void *arg,
                                              esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance);

// ---- esp_netif ----
typedef struct test_netif esp_netif_t;
typedef struct { int unused; } esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){ 0 })
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xFFu), (int)(((a)->addr >> 8) & 0xFFu), \
                  (int)(((a)->addr >> 16) & 0xFFu), (int)(((a)->addr >> 24) & 0xFFu)
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info);

// ---- esp_wifi ----
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ 0 })
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_IF_STA 0
typedef struct {
    struct {
        char ssid[33];
        char password[65];
        int listen_interval;
    } sta;
} wifi_config_t;
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
typedef struct { int rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *conf);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
void esp_netif_destroy_default_wifi(esp_netif_t *netif);
// SDK:默认 netif 全部注销后清空默认 legacy handlers;netif 参数用于
// "disconnect_and_destroy"(残点清理场景传未 attach 的新 netif)。
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(void *esp_netif);

// ---- esp_sntp ----
#define ESP_SNTP_OPMODE_POLL 0
typedef void (*esp_sntp_sync_cb_t)(struct timeval *tv);
void esp_sntp_setoperatingmode(int mode);
void esp_sntp_setservername(unsigned idx, const char *host);
void esp_sntp_set_time_sync_notification_cb(esp_sntp_sync_cb_t cb);
void esp_sntp_init(void);
void esp_sntp_stop(void);

// ---- 堆诊断(行为断言不依赖具体数值) ----
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t esp_get_free_heap_size(void);

// ---- 测试控制面 ----

// 起步失败注入点(每次触发后自动清除,可对重试逐次注入)。
enum {
    CONN_FAIL_NONE = 0,
    // 启动阶段(队列 → 锁 → netif 初始化 → 事件循环 → 事件处理器 → 任务)
    CONN_FAIL_QUEUE_CREATE,
    CONN_FAIL_LOCK_CREATE,
    CONN_FAIL_NETIF_INIT,
    CONN_FAIL_EVENT_LOOP,
    CONN_FAIL_HANDLER_WIFI,
    CONN_FAIL_HANDLER_IP,
    CONN_FAIL_TASK_CREATE,
    // Wi-Fi 栈七阶段(任务内),顺序保持:回归按其区间遍历。
    CONN_FAIL_NETIF_NEW,
    CONN_FAIL_NETIF_ATTACH,
    CONN_FAIL_DEFAULT_HANDLERS,
    CONN_FAIL_WIFI_INIT,
    CONN_FAIL_WIFI_STORAGE,
    CONN_FAIL_WIFI_MODE,
    CONN_FAIL_WIFI_START,
};

typedef struct {
    unsigned netif_new, netif_new_fail, netif_attach, netif_attach_fail;
    unsigned netif_destroy;
    unsigned defaults_set, defaults_fail;
    // 默认 legacy handlers(ESP-IDF 5.5.3 语义,见 conn_runtime.c 模型):
    unsigned defaults_clear_calls;      // clear_default_wifi_handlers 执行次数
    unsigned defaults_clear_alloc_fail; // 锁忙 + legacy context 分配失败(残点)
    unsigned defaults_marked_busy_ok;   // 已标记残点 + 锁忙:返回 OK 但不摘除
    unsigned defaults_mark_cleanup;     // 未标记 + 锁忙 + 分配成功:标记并投递清理
    unsigned defaults_sweep_calls;      // 清理入口调用次数(生产 attach 前)
    unsigned defaults_regs;             // legacy 注册新增的节点数
    unsigned defaults_dup_regs;         // 同函数 legacy 再注册(匹配既有节点)
    unsigned defaults_freed;            // 摘除的节点数(含已禁用残点)
    unsigned defaults_invocations;      // 默认处理器被派发调用次数
    unsigned defaults_netif_start;      // STA_START → 默认 netif 启动动作
    unsigned defaults_dhcp_start;       // STA_CONNECTED → 默认 DHCP 启动动作
    unsigned defaults_got_ip;           // GOT_IP → 默认动作
    unsigned wifi_init, wifi_init_fail, wifi_deinit;
    unsigned wifi_start, wifi_start_fail, wifi_stop;
    unsigned wifi_connect, wifi_disconnect, wifi_set_config;
    unsigned sntp_init, sntp_stop;
    unsigned handler_regs, handler_unregs;
    unsigned handler_reg_fail;      // 注册失败次数
    unsigned handler_duplicate;     // 同一处理器重复注册(应为 0)
    unsigned handler_unreg_missing; // 非法实例句柄注销(应为 0)
    unsigned handler_unreg_marked;  // 延迟注销标记(NO_MEM)次数
    unsigned handler_invocations;   // 事件派发实际调用回调次数
    unsigned queue_create, queue_delete, queue_create_fail;
    unsigned mutex_create, mutex_create_fail, mutex_delete;
    unsigned task_create_fail;
    unsigned netif_init_calls, netif_init_fail;
    unsigned event_loop_created, event_loop_existing, event_loop_fail;
    bool netif_live, driver_live, defaults_live;
    bool wifi_live, wifi_running;
} conn_observed_t;

const conn_observed_t *test_conn_observed(void);
unsigned test_conn_fail_stage(void);
// 本场景内新建的存活资源数(失败回滚后必须回到 0)。
unsigned test_conn_live_queues(void);
unsigned test_conn_live_mutexes(void);
unsigned test_conn_live_handlers(void);
// 默认事件循环是否已创建(进程级共享单例,不随启动失败回收)。
bool test_conn_event_loop_live(void);
// 已被标记但尚未释放的处理器节点数(仍计入 live;回调已禁用)。
unsigned test_conn_marked_handlers(void);
// 默认 legacy handlers 的节点数(含已禁用残点)与残点数。
unsigned test_conn_defaults_live(void);
unsigned test_conn_defaults_marked(void);
// 延迟注销语义注入(ESP-IDF 5.5.3,已按 esp_event 源码核对):开启后,
// 对该 base 的注销走"事件循环锁忙 + 清理事件分配失败"路径——未标记
// 节点被标记(回调禁用)并返回 ESP_ERR_NO_MEM且节点保留;已标记节点
// 再次注销返回 OK 但不摘除(锁忙空操作)。关闭后注销立即摘除。
void test_conn_set_unreg_fault(esp_event_base_t base, bool on);
// 默认 handlers 清理故障注入(单次,自动解除):下一次默认清理中
// STA_START 的 legacy 注销走"锁忙 + 清理上下文分配失败"路径——节点被
// 标记禁用且保留、无清理事件;其余默认处理器按锁空闲路径摘除
// (与复审复现日志一致)。
void test_conn_set_defaults_clear_fault(bool on);
// 默认 handlers 清理的"事件循环锁忙"注入(持续,直到解除):开启后每一次
// legacy 注销都走锁忙分支——未标记节点先标记再分配清理事件上下文
// (分配失败见上面的单次注入),已标记节点返回 ESP_OK 但不摘除、不分配、
// 不产生清理事件(SDK:components/esp_event/esp_event.c:516-521,879-886)。
// 分配成功时清理事件已投递,桩在本次清理返回前建模摘除;分配失败留下的
// 禁用残点只有锁空闲清理才能摘除(见 conn_runtime.c 的模型说明)。
void test_conn_set_defaults_clear_busy(bool on);

// 场景之间的完整复位(要求上一场景的任务已退出、资源已清零)。
void test_conn_reset(void);
// 注入一次指定起步失败的调用;CONN_FAIL_NONE 取消注入。
void test_conn_fail_at(int stage);
// 凭据桩。
void test_conn_set_creds(const char *ssid, const char *pass);
void test_conn_clear_creds(void);
// STA 当前 IP(0=无);按 IP2STR 的字节序给出可读地址。
void test_conn_set_ip(uint32_t addr);
// 触发已注册的 WIFI_EVENT 处理器(id=WIFI_EVENT_STA_START/DISCONNECTED)。
void test_conn_wifi_event(int32_t id, void *data);
// 只派发默认 legacy handlers(不调用应用实例处理器):用于停栈状态下
// "残点仍被禁用"的语义探针——此刻驱动不会再产生真实事件,完整事件
// 派发(实例 + 默认两条路径)在恢复后的用例里验证。
void test_conn_default_dispatch(int32_t id);
// 触发 IP_EVENT_STA_GOT_IP 处理器。
void test_conn_ip_got_ip(void);
// 触发 SNTP 同步回调(与真实回调同一实现)。
void test_conn_sntp_fire(int64_t epoch);

// 等待 conn_task 停靠在下一次队列接收或结束;返回本次请求的等待时长
// (ms);任务结束返回 0。
unsigned long test_conn_wait_park(void);
// 队列为空断言后按"退避超时"恢复执行。
void test_conn_resume_timeout(void);
// 队列非空断言后按"事件到达"恢复执行。
void test_conn_resume_event(void);
bool test_conn_finished(void);

// 读侧交错钩子:在 esp_netif_get_ip_info 桩内、持有读者锁时调用一次
// (用于"读者已取得接口"与"销毁发生"的交错顺序回归)。
void test_conn_set_get_ip_info_hook(void (*hook)(void));
// 有界等待销毁流程执行到 esp_wifi_deinit(即已越过驱动停止,正在等待
// netif 锁/销毁);超时返回 false。
bool test_conn_wait_deinit(unsigned long timeout_ms);

// 最近一次 esp_wifi_set_config 的内容(判断新凭据是否真正生效)。
const char *test_conn_last_ssid(void);
const char *test_conn_last_pass(void);
