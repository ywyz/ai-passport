// tests/ble_stubs/prov_runtime.c —— suite_prov_ble.c 主机测试的可控运行时。
//
// prov_task 运行在真实线程上;"任务停靠"发生在 xQueueReceive 需要等待时,
// 测试逐次放行("200ms 超时"或"事件到达")。GAP/GATT 回调由测试直接驱动。
//
// 并发交错由独立的 host 线程 + "通知提交闸门"构造(见下方提交出口):
//   - 发送线程(prov 任务)在闸门处停住时仍持有生产的生命周期锁;
//   - host 线程按真实顺序执行"旧连接表项删除 → DISCONNECT 回调 →
//     新连接表项插入 → CONNECT 回调 → 订阅/MTU",全部在同一个线程上;
//   - 生产把"终校验 + 提交"与 GAP 生命周期放进同一串行化边界时,host
//     的 GAP 回调会阻塞在该锁上,换机完成必然晚于本次提交;否则换机在
//     提交前生效,断言(实际接收连接/MTU/帧内容与顺序计数)直接失败。
// 桩不查询"生产是否持锁"来决定调度,危险窗口的打开完全由测试驱动。
//
// 本文件同时复现两类"真实环境语义",避免测试替身替生产代码做判定:
//   - 扫描结果所有权:驱动动态持有结果,只能由 get_ap_records(消费)或
//     clear_ap_list(丢弃)释放;
//   - 通知提交的串行化:提交按连接表解析句柄,句柄复用会把旧帧送到新连接。
#include "prov_test_stubs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define TEST_PQ_DEPTH 8
#define TEST_PQ_ITEM_MAX 260  // pq_item_t(约 252B)+ 余量
#define SCAN_TEST_MAX_APS 40
#define TEST_MAX_SEMS 8
#define TEST_PROV_CONN_MAX 8
// 与 suite_prov_proto.h 的 SUITE_PROV_MTU_DEFAULT 一致;桩不依赖生产头文件。
#define TEST_PROV_MTU_DEFAULT 23u

// 桩内的 BLE_HS_ENOTCONN:提交时按句柄查不到连接(与真实栈一致:释放 mbuf)。
#define TEST_PROV_ENOTCONN 30

enum task_state {
    PT_IDLE = 0,   // 已创建、尚未开始运行
    PT_RUNNING,    // 任务线程正在执行
    PT_PARKED,     // 任务线程停在一次队列等待,等待测试命令
    PT_FINISHED,   // 任务线程已退出
};

struct test_queue {
    unsigned depth;
    unsigned item_size;
    unsigned count;
    unsigned head;
    uint8_t items[TEST_PQ_DEPTH][TEST_PQ_ITEM_MAX];
};

struct test_task {
    TaskFunction_t entry;
    void *arg;
};

// 二值信号量(任务退出握手)或互斥量(生产的发送串行化边界)。
// 生产用 xSemaphoreCreateMutex():FreeRTOS 普通互斥量,非递归;桩用
// mtx_plain 并显式拒绝同线程重入,不掩盖锁语义错误。
struct test_sem {
    bool in_use;
    bool is_mutex;
    bool available;   // 二值信号量:已 give 未 take
    mtx_t lock;       // 互斥量
    thrd_t owner;
    unsigned depth;
};

static mtx_t s_lock;
static cnd_t s_cv;
static once_flag s_runtime_once = ONCE_FLAG_INIT;

static struct test_queue s_queue;
static bool s_queue_live;
static struct test_task s_task;
static struct test_sem s_sems[TEST_MAX_SEMS];
static unsigned s_sem_count;
static thrd_t s_thread;
static enum task_state s_state = PT_IDLE;
static bool s_task_created, s_finished, s_joined, s_resume_timeout;
static bool s_task_delete_requested;
static bool s_auto_wake;
static _Thread_local bool t_in_task;
static unsigned long s_wait_ms;
static uint32_t s_now_ms;

// ---- 通知提交闸门与行为顺序计数(双线程交错控制) ----
static bool s_notify_gate_armed;
static unsigned s_notify_gate_skip;   // 放行前跳过的提交次数
static bool s_notify_gate_entered;    // 发送线程已停在闸门内
static bool s_notify_gate_released;   // 测试已放行
static uint32_t s_seq;                // 行为顺序计数器
static uint32_t s_notify_gate_seq;    // 发送线程通过闸门时的序号

static mtx_t s_port_mux;

static void (*s_scan_hook)(void);
static wifi_ap_record_t s_scan_aps[SCAN_TEST_MAX_APS];
static unsigned s_scan_ap_count;
static bool s_scan_start_fail;
static bool s_scan_get_fail;
static prov_scan_stats_t s_scan_stats;
static wifi_ap_record_t *s_scan_results;  // 驱动动态持有的扫描结果
static unsigned s_scan_live_allocs;

// 连接表模型(句柄 → 当前连接序号/ATT MTU)。
static bool s_conn_live[TEST_PROV_CONN_MAX];
static unsigned s_conn_serial[TEST_PROV_CONN_MAX];
static unsigned s_conn_mtu[TEST_PROV_CONN_MAX];
static unsigned s_conn_serial_next;

static void (*s_frame_sink)(uint16_t conn, unsigned conn_serial,
                            unsigned att_mtu, const uint8_t *frame, size_t len);
static void (*s_precommit_hook)(void);
static unsigned s_terminate_calls;
static unsigned s_mbuf_live;

static void runtime_init(void)
{
    assert(mtx_init(&s_lock, mtx_plain) == thrd_success);
    assert(cnd_init(&s_cv) == thrd_success);
    assert(mtx_init(&s_port_mux, mtx_recursive) == thrd_success);
}

static void test_lock(void)
{
    call_once(&s_runtime_once, runtime_init);
    mtx_lock(&s_lock);
}

static void test_unlock(void)
{
    mtx_unlock(&s_lock);
}

void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "stub error";
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 32 * 1024;
}

uint32_t esp_get_free_heap_size(void)
{
    return 128 * 1024;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = { .version = "test-0.0" };
    return &desc;
}

void test_port_enter(portMUX_TYPE *mux)
{
    (void)mux;
    call_once(&s_runtime_once, runtime_init);
    int rc = mtx_lock(&s_port_mux);
    assert(rc == thrd_success);
}

void test_port_exit(portMUX_TYPE *mux)
{
    (void)mux;
    int rc = mtx_unlock(&s_port_mux);
    assert(rc == thrd_success);
}

// ---- 线程化任务与队列 ----

static int task_trampoline(void *arg)
{
    struct test_task *task = arg;
    test_lock();
    t_in_task = true;
    s_state = PT_RUNNING;
    test_unlock();

    task->entry(task->arg);
    assert(!"prov task returned instead of exiting");
    return 0;
}

// 任务线程:记录本次等待时长后停靠,由测试决定恢复方式。
static void task_park(unsigned long wait_ms)
{
    assert(t_in_task);
    s_wait_ms = wait_ms;
    s_state = PT_PARKED;
    cnd_broadcast(&s_cv);
    while (s_state == PT_PARKED) cnd_wait(&s_cv, &s_lock);
}

QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size)
{
    test_lock();
    assert(depth <= TEST_PQ_DEPTH && item_size <= TEST_PQ_ITEM_MAX);
    assert(!s_task_created || s_finished);
    s_queue.depth = depth;
    s_queue.item_size = item_size;
    s_queue.count = 0;
    s_queue.head = 0;
    s_queue_live = true;
    test_unlock();
    return &s_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    test_lock();
    assert(queue == &s_queue && item);
    if (s_queue.count >= s_queue.depth) {
        test_unlock();
        return pdFALSE;  // 满:生产按丢弃/背压处理
    }
    unsigned slot = (s_queue.head + s_queue.count) % TEST_PQ_DEPTH;
    memcpy(s_queue.items[slot], item, s_queue.item_size);
    s_queue.count++;
    // 真实 FreeRTOS:发送唤醒阻塞中的接收者。默认关闭保持逐次放行的
    // 确定性;配网页退出等需要真实唤醒的路径显式开启。
    if (s_auto_wake && s_state == PT_PARKED) {
        s_resume_timeout = false;
        s_state = PT_RUNNING;
        cnd_broadcast(&s_cv);
    }
    test_unlock();
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t ticks)
{
    test_lock();
    assert(queue == &s_queue && out);
    if (s_queue.count == 0 && ticks != 0) {
        assert(t_in_task);
        task_park((unsigned long)ticks);
        if (s_resume_timeout) {
            s_resume_timeout = false;
            assert(s_queue.count == 0);
            test_unlock();
            return pdFALSE;
        }
        assert(s_queue.count > 0);  // 事件到达
    }
    if (s_queue.count == 0) {
        test_unlock();
        return pdFALSE;
    }
    memcpy(out, s_queue.items[s_queue.head], s_queue.item_size);
    s_queue.head = (s_queue.head + 1) % TEST_PQ_DEPTH;
    s_queue.count--;
    test_unlock();
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    test_lock();
    assert(queue == &s_queue);
    s_queue = (struct test_queue){ 0 };
    s_queue_live = false;
    test_unlock();
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority;
    test_lock();
    assert(entry && task && !s_task_created);
    s_task = (struct test_task){ .entry = entry, .arg = arg };
    s_task_created = true;
    s_state = PT_IDLE;
    s_finished = false;
    s_joined = false;
    s_task_delete_requested = false;
    *task = &s_task;
    test_unlock();
    // 真实线程:任务体立即开始运行,停在第一次队列等待时由测试放行。
    assert(thrd_create(&s_thread, task_trampoline, &s_task) == thrd_success);
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) {
        // 任务自删(生产不依赖该路径,保留 FreeRTOS 语义)。
        test_lock();
        assert(t_in_task);
        s_finished = true;
        s_state = PT_FINISHED;
        cnd_broadcast(&s_cv);
        test_unlock();
        thrd_exit(0);
    }
    // 外部删除(suite_prov_ble_end):任务已交出退出信号量并停在自己的
    // 删除等待循环(vTaskDelay)里,由本调用置位后退出线程。
    test_lock();
    assert(task == &s_task && s_task_created);
    s_task_delete_requested = true;
    test_unlock();
    assert(thrd_join(s_thread, NULL) == thrd_success);
    test_lock();
    s_task_created = false;
    s_joined = true;
    test_unlock();
}

void vTaskDelay(TickType_t ticks)
{
    test_lock();
    assert(t_in_task);
    if (s_task_delete_requested) {
        s_finished = true;
        s_state = PT_FINISHED;
        cnd_broadcast(&s_cv);
        test_unlock();
        thrd_exit(0);
    }
    s_now_ms += ticks;  // 虚拟时钟:帧间排空延时
    test_unlock();
}

TickType_t xTaskGetTickCount(void)
{
    return s_now_ms;
}

// ---- 信号量与互斥量 ----

static struct test_sem *sem_alloc(bool is_mutex)
{
    for (unsigned i = 0; i < TEST_MAX_SEMS; i++) {
        if (!s_sems[i].in_use) {
            s_sems[i] = (struct test_sem){ .in_use = true,
                                           .is_mutex = is_mutex };
            if (i >= s_sem_count) s_sem_count = i + 1;
            return &s_sems[i];
        }
    }
    assert(!"semaphore pool exhausted");
    return NULL;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    test_lock();
    struct test_sem *sem = sem_alloc(false);
    sem->available = false;
    test_unlock();
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    test_lock();
    struct test_sem *sem = sem_alloc(true);
    // 生产用 xSemaphoreCreateMutex():普通(非递归)互斥量。
    assert(mtx_init(&sem->lock, mtx_plain) == thrd_success);
    test_unlock();
    return sem;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    test_lock();
    assert(sem && sem->in_use);
    BaseType_t result;
    if (sem->is_mutex) {
        assert(sem->depth == 1 && thrd_equal(sem->owner, thrd_current()));
        sem->depth = 0;
        result = pdTRUE;
        // 记完账再解锁:持测试锁时不做可能唤醒等待者的操作,
        // 避免"测试锁 ↔ 生命周期锁"交叉等待。
        test_unlock();
        assert(mtx_unlock(&sem->lock) == thrd_success);
        return result;
    }
    result = sem->available ? pdFALSE : pdTRUE;
    sem->available = true;
    cnd_broadcast(&s_cv);
    test_unlock();
    return result;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    test_lock();
    assert(sem && sem->in_use);
    if (sem->is_mutex) {
        // 普通互斥量语义:同一线程重入在 FreeRTOS 上会死锁,桩直接断言
        // 失败(fail fast),而不是用递归互斥量静默放行锁使用错误。
        // 等待互斥量时不持测试锁,避免与"持锁后调用桩"的顺序交叉等待。
        bool reentered = sem->depth > 0 &&
                         thrd_equal(sem->owner, thrd_current());
        test_unlock();
        assert(!reentered);
        assert(mtx_lock(&sem->lock) == thrd_success);
        test_lock();
        sem->owner = thrd_current();
        sem->depth = 1;
        test_unlock();
        return pdTRUE;
    }
    if (sem->available) {
        sem->available = false;
        test_unlock();
        return pdTRUE;
    }
    if (ticks == 0) {
        test_unlock();
        return pdFALSE;
    }
    struct timespec deadline;
    int rc = thrd_success;
    if (ticks != portMAX_DELAY) {
        assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
        deadline.tv_sec += (time_t)(ticks / 1000);
        deadline.tv_nsec += (long)(ticks % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    while (!sem->available && rc != thrd_timedout) {
        rc = (ticks == portMAX_DELAY) ? cnd_wait(&s_cv, &s_lock)
                                      : cnd_timedwait(&s_cv, &s_lock, &deadline);
    }
    BaseType_t out = pdFALSE;
    if (sem->available) {
        sem->available = false;
        out = pdTRUE;
    }
    test_unlock();
    return out;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    test_lock();
    assert(sem && sem->in_use);
    if (sem->is_mutex) {
        assert(sem->depth == 0);
        mtx_destroy(&sem->lock);
    }
    sem->in_use = false;
    test_unlock();
}

// ---- NimBLE / GATT 桩 ----

struct ble_hs_cfg_t ble_hs_cfg;

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields)
{
    (void)fields;
    return 0;
}

int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *fields)
{
    (void)fields;
    return 0;
}

int ble_gap_adv_start(uint8_t own_addr_type, const void *direct_addr,
                      int32_t duration_ms, const struct ble_gap_adv_params *params,
                      int (*cb)(struct ble_gap_event *event, void *arg),
                      void *cb_arg)
{
    (void)own_addr_type; (void)direct_addr; (void)duration_ms;
    (void)params; (void)cb; (void)cb_arg;
    return 0;
}

int ble_gap_adv_stop(void) { return 0; }

int ble_gap_terminate(uint16_t conn_handle, int reason)
{
    (void)conn_handle;
    (void)reason;
    test_lock();
    s_terminate_calls++;
    test_unlock();
    return 0;
}

int ble_hs_util_ensure_addr(int prefer_random)
{
    (void)prefer_random;
    return 0;
}

int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type)
{
    (void)privacy;
    if (out_addr_type) *out_addr_type = 0;
    return 0;
}

int ble_hs_id_copy_addr(uint8_t addr_type, uint8_t *out_addr, int *out_addr_len)
{
    (void)addr_type;
    if (out_addr) memset(out_addr, 0x11, 6);
    if (out_addr_len) *out_addr_len = 6;
    return 0;
}

int ble_att_set_preferred_mtu(uint16_t mtu)
{
    (void)mtu;
    return 0;
}

int ble_svc_gap_init(void) { return 0; }
int ble_svc_gatt_init(void) { return 0; }
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *svcs)
{
    (void)svcs;
    return 0;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *svcs)
{
    (void)svcs;
    return 0;
}
int ble_svc_gap_device_name_set(const char *name)
{
    (void)name;
    return 0;
}

// 指针相等即相同 UUID:测试用生产静态 UUID 构造访问上下文。
int ble_uuid_cmp(const ble_uuid_t *uuid1, const ble_uuid_t *uuid2)
{
    return uuid1 == uuid2 ? 0 : 1;
}

esp_err_t nimble_port_init(void) { return ESP_OK; }
void nimble_port_run(void) {}
void nimble_port_freertos_init(TaskFunction_t host_fn) { (void)host_fn; }
void nimble_port_freertos_deinit(void) {}

int os_mbuf_append(struct os_mbuf *m, const void *data, uint16_t len)
{
    assert(m && m->len + len <= SUITE_PROV_MBUF_DATA);
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return 0;
}

void os_mbuf_free_chain(struct os_mbuf *om)
{
    test_lock();
    assert(om && s_mbuf_live > 0);
    s_mbuf_live--;
    test_unlock();
    free(om);
}

struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len)
{
    assert(buf && len <= SUITE_PROV_MBUF_DATA);
    struct os_mbuf *om = malloc(sizeof(*om));
    assert(om);
    om->len = len;
    memcpy(om->data, buf, len);
    test_lock();
    s_mbuf_live++;
    test_unlock();
    // 提交前钩子只在任务线程内触发(测试构造 RX 帧也会经过本函数)。
    if (t_in_task) {
        void (*hook)(void);
        test_lock();
        hook = s_precommit_hook;
        s_precommit_hook = NULL;
        test_unlock();
        if (hook) hook();
    }
    return om;
}

int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len,
                        uint16_t *out_copy_len)
{
    assert(om && flat && out_copy_len);
    uint16_t len = om->len < max_len ? om->len : max_len;
    memcpy(flat, om->data, len);
    *out_copy_len = len;
    return len == om->len ? 0 : 1;
}

// 提交出口:复现真实栈的语义。
//   1) 闸门(仅测试武装时):在"按连接表解析句柄"之前停住,等待测试放行。
//      发送线程在闸门内仍持有生产的生命周期锁;测试从这个确定点启动
//      独立 host 线程执行换机/reset,不做任何"生产是否持锁"的探针;
//   2) 按连接表解析句柄:未连接则释放 mbuf 并返回 ENOTCONN(同 ble_att_tx),
//      否则把帧交给该连接,并记录该连接的序号与 ATT MTU。
int ble_gatts_notify_custom(uint16_t conn_handle, uint16_t att_handle,
                            struct os_mbuf *om)
{
    (void)att_handle;
    assert(om);

    test_lock();
    if (s_notify_gate_armed) {
        if (s_notify_gate_skip > 0) {
            s_notify_gate_skip--;
        } else {
            s_notify_gate_armed = false;
            s_notify_gate_entered = true;
            cnd_broadcast(&s_cv);
            // 等待期间不持测试锁;host 线程的 GAP 回调由生产锁决定
            // 能否与本次提交交错,桩不介入调度。
            while (!s_notify_gate_released) cnd_wait(&s_cv, &s_lock);
            s_notify_gate_released = false;
            s_notify_gate_entered = false;
            s_notify_gate_seq = ++s_seq;
        }
    }
    bool live = conn_handle < TEST_PROV_CONN_MAX && s_conn_live[conn_handle];
    unsigned serial = live ? s_conn_serial[conn_handle] : 0;
    unsigned mtu = live ? s_conn_mtu[conn_handle] : 0;
    void (*sink)(uint16_t, unsigned, unsigned, const uint8_t *, size_t) =
        s_frame_sink;
    test_unlock();

    if (!live) {
        os_mbuf_free_chain(om);
        return TEST_PROV_ENOTCONN;
    }
    if (sink) sink(conn_handle, serial, mtu, om->data, om->len);
    os_mbuf_free_chain(om);  // 与 NimBLE 一致:调用后协议栈不再持有
    return 0;
}

// ---- esp_wifi 扫描桩(驱动动态所有权) ----

static void scan_release(void)
{
    assert(s_scan_live_allocs > 0 && s_scan_results);
    free(s_scan_results);
    s_scan_results = NULL;
    s_scan_live_allocs--;
    s_scan_stats.frees++;
    s_scan_stats.live = false;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)config;
    assert(block && t_in_task);  // 生产只做阻塞扫描
    void (*hook)(void);
    test_lock();
    s_scan_stats.start_calls++;
    if (s_scan_start_fail) {
        s_scan_start_fail = false;
        s_scan_stats.start_fail++;
        test_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    // 上一次结果未被消费/丢弃就再次扫描:驱动会覆盖它。记录该事件并
    // 释放,便于测试断言"每条路径都不遗留扫描结果"。
    if (s_scan_results) {
        s_scan_stats.overwritten++;
        scan_release();
    }
    hook = s_scan_hook;
    s_scan_hook = NULL;
    test_unlock();
    if (hook) hook();

    test_lock();
    // 驱动在扫描成功后动态分配结果集;只有 get/clear 能释放。
    s_scan_results = calloc(s_scan_ap_count ? s_scan_ap_count : 1,
                            sizeof(wifi_ap_record_t));
    assert(s_scan_results);
    memcpy(s_scan_results, s_scan_aps,
           s_scan_ap_count * sizeof(wifi_ap_record_t));
    s_scan_live_allocs++;
    s_scan_stats.allocs++;
    s_scan_stats.live = true;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number,
                                       wifi_ap_record_t *ap_records)
{
    test_lock();
    s_scan_stats.get_calls++;
    assert(number && ap_records);
    if (!s_scan_results) {
        // 结果已被消费/丢弃:真实栈语义为"无结果可取"。
        test_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_get_fail) {
        // 获取失败:结果仍由驱动持有,必须由调用方 clear,否则泄漏。
        s_scan_get_fail = false;
        s_scan_stats.get_fail++;
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    unsigned count = s_scan_ap_count;
    if (count > *number) count = *number;
    memcpy(ap_records, s_scan_results, count * sizeof(wifi_ap_record_t));
    *number = (uint16_t)count;
    scan_release();  // 消费即释放(ESP-IDF 5.5.3)
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_clear_ap_list(void)
{
    test_lock();
    s_scan_stats.clear_calls++;
    if (s_scan_results) {
        scan_release();
    }
    // 结果不存在时是安全空操作(不重复释放)。
    test_unlock();
    return ESP_OK;
}

// ---- 测试控制面 ----

unsigned long test_prov_wait_park(void)
{
    test_lock();
    while (s_state == PT_IDLE || s_state == PT_RUNNING) {
        cnd_wait(&s_cv, &s_lock);
    }
    unsigned long wait = 0;
    if (s_state == PT_FINISHED) {
        if (!s_joined) {
            s_joined = true;
            test_unlock();
            assert(thrd_join(s_thread, NULL) == thrd_success);
            test_lock();
        }
    } else {
        assert(s_state == PT_PARKED);
        wait = s_wait_ms;
    }
    test_unlock();
    return wait;
}

void test_prov_resume_timeout(void)
{
    test_lock();
    assert(s_state == PT_PARKED);
    assert(s_queue.count == 0);
    s_resume_timeout = true;
    s_state = PT_RUNNING;
    cnd_broadcast(&s_cv);
    test_unlock();
}

void test_prov_resume_event(void)
{
    test_lock();
    assert(s_state == PT_PARKED);
    assert(s_queue.count > 0);
    s_resume_timeout = false;
    s_state = PT_RUNNING;
    cnd_broadcast(&s_cv);
    test_unlock();
}

bool test_prov_finished(void)
{
    test_lock();
    bool finished = s_finished;
    test_unlock();
    return finished;
}

void test_prov_set_auto_wake(bool on)
{
    test_lock();
    s_auto_wake = on;
    test_unlock();
}

void test_prov_set_scan_hook(void (*hook)(void))
{
    test_lock();
    s_scan_hook = hook;
    test_unlock();
}

void test_prov_set_scan_aps(const wifi_ap_record_t *aps, unsigned count)
{
    test_lock();
    assert(count <= SCAN_TEST_MAX_APS);
    // 零元素 = 清空设置列表:不执行 memcpy,源指针允许为 NULL(不解引用);
    // 非零数量必须提供有效源指针。空扫描的"成功返回 data.nets=[]"契约
    // 由调用方用例继续验证。
    if (count > 0) {
        assert(aps != NULL);
        memcpy(s_scan_aps, aps, count * sizeof(wifi_ap_record_t));
    }
    s_scan_ap_count = count;
    test_unlock();
}

const prov_scan_stats_t *test_prov_scan_stats(void)
{
    return &s_scan_stats;
}

void test_prov_set_scan_start_fail(bool fail)
{
    test_lock();
    s_scan_start_fail = fail;
    test_unlock();
}

void test_prov_set_scan_get_fail(bool fail)
{
    test_lock();
    s_scan_get_fail = fail;
    test_unlock();
}

unsigned test_prov_conn_attach(uint16_t conn)
{
    test_lock();
    assert(conn < TEST_PROV_CONN_MAX && !s_conn_live[conn]);
    s_conn_live[conn] = true;
    s_conn_mtu[conn] = TEST_PROV_MTU_DEFAULT;
    unsigned serial = ++s_conn_serial_next;
    s_conn_serial[conn] = serial;
    test_unlock();
    return serial;
}

void test_prov_conn_detach(uint16_t conn)
{
    test_lock();
    assert(conn < TEST_PROV_CONN_MAX && s_conn_live[conn]);
    s_conn_live[conn] = false;
    s_conn_serial[conn] = 0;
    test_unlock();
}

unsigned test_prov_conn_serial(uint16_t conn)
{
    test_lock();
    unsigned serial = (conn < TEST_PROV_CONN_MAX && s_conn_live[conn])
                          ? s_conn_serial[conn]
                          : 0;
    test_unlock();
    return serial;
}

void test_prov_conn_set_mtu(uint16_t conn, unsigned mtu)
{
    test_lock();
    assert(conn < TEST_PROV_CONN_MAX && s_conn_live[conn]);
    s_conn_mtu[conn] = mtu;
    test_unlock();
}

unsigned test_prov_conn_mtu(uint16_t conn)
{
    test_lock();
    unsigned mtu = (conn < TEST_PROV_CONN_MAX && s_conn_live[conn])
                       ? s_conn_mtu[conn]
                       : 0;
    test_unlock();
    return mtu;
}

void test_prov_set_frame_sink(void (*sink)(uint16_t conn, unsigned conn_serial,
                                           unsigned att_mtu,
                                           const uint8_t *frame, size_t len))
{
    test_lock();
    s_frame_sink = sink;
    test_unlock();
}

void test_prov_set_precommit_hook(void (*hook)(void))
{
    test_lock();
    s_precommit_hook = hook;
    test_unlock();
}

// ---- 通知提交闸门(双线程交错控制) ----
//
// 危险窗口的打开只由测试决定:武装后,第 skip+1 次 notify 在按连接表
// 解析句柄之前停住并广播,测试从该确定点启动 host 线程执行换机/reset,
// 再由测试放行。桩不再查询生产是否持锁,也不代替测试决定注入时机。

void test_prov_arm_notify_gate(unsigned skip)
{
    test_lock();
    assert(!s_notify_gate_armed);
    s_notify_gate_armed = true;
    s_notify_gate_skip = skip;
    s_notify_gate_released = false;
    test_unlock();
}

bool test_prov_wait_notify_gate(unsigned long timeout_ms)
{
    test_lock();
    bool entered = s_notify_gate_entered;
    if (!entered) {
        struct timespec deadline;
        assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
        deadline.tv_sec += (time_t)(timeout_ms / 1000);
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        int rc = thrd_success;
        while (!s_notify_gate_entered && rc != thrd_timedout) {
            rc = cnd_timedwait(&s_cv, &s_lock, &deadline);
        }
        entered = s_notify_gate_entered;
    }
    test_unlock();
    return entered;
}

void test_prov_release_notify_gate(void)
{
    test_lock();
    assert(s_notify_gate_entered);
    s_notify_gate_released = true;
    cnd_broadcast(&s_cv);
    test_unlock();
}

// 行为顺序计数:host 线程完成换机序列、发送线程通过闸门各取一个序号。
uint32_t test_prov_seq_bump(void)
{
    test_lock();
    uint32_t seq = ++s_seq;
    test_unlock();
    return seq;
}

uint32_t test_prov_gate_seq(void)
{
    test_lock();
    uint32_t seq = s_notify_gate_seq;
    test_unlock();
    return seq;
}

unsigned test_prov_live_queues(void)
{
    test_lock();
    unsigned live = s_queue_live ? 1u : 0u;
    test_unlock();
    return live;
}

unsigned test_prov_live_sems(void)
{
    test_lock();
    unsigned live = 0;
    for (unsigned i = 0; i < TEST_MAX_SEMS; i++) {
        if (s_sems[i].in_use) live++;
    }
    test_unlock();
    return live;
}

unsigned test_prov_live_tasks(void)
{
    test_lock();
    unsigned live = s_task_created ? 1u : 0u;
    test_unlock();
    return live;
}

unsigned test_prov_live_mbufs(void)
{
    test_lock();
    unsigned live = s_mbuf_live;
    test_unlock();
    return live;
}

unsigned test_prov_terminate_calls(void)
{
    test_lock();
    unsigned calls = s_terminate_calls;
    test_unlock();
    return calls;
}

// ---- 应用侧分配失败注入 ----
//
// 测试通过 "#define calloc test_prov_calloc" 只拦截生产源码的 calloc
// (桩与真实 cJSON 在此之前编译,使用真实分配器),用于覆盖
// cmd_wifi_scan 的应用侧分配失败路径。
static unsigned s_calloc_calls;
static unsigned s_calloc_fail_at;  // 1-based;0 = 不失败

void test_prov_fail_calloc_at(unsigned nth)
{
    test_lock();
    s_calloc_calls = 0;  // 从本次注入开始计数
    s_calloc_fail_at = nth;
    test_unlock();
}

void *test_prov_calloc(size_t count, size_t size)
{
    // 与严格堆实现一致:零元素分配返回 NULL。生产不得把 calloc(0) 的
    // NULL 当作内存不足(零 AP 扫描仍要成功返回空列表)。
    if (count == 0 || size == 0) return NULL;
    test_lock();
    s_calloc_calls++;
    bool fail = s_calloc_fail_at != 0 && s_calloc_calls == s_calloc_fail_at;
    if (fail) s_calloc_fail_at = 0;
    test_unlock();
    if (fail) return NULL;
    return calloc(count, size);
}
