// tests/conn_stubs/conn_runtime.c —— suite_connection.c 主机测试的可控运行时。
//
// conn_task 运行在真实线程上;"任务停靠"发生在 xQueueReceive 需要等待时。
// 测试逐次放行:要么按"退避超时"(队列必须为空),要么按"事件到达"恢复。
// 网络栈每一步调用都可注入失败,并观察资源标志/计数器以验证完整回滚。
#include "conn_test_stubs.h"
#include "suite_storage.h"

#include <stdio.h>
#include <string.h>
#include <threads.h>

#define TEST_QUEUE_MAX_DEPTH 8
#define TEST_QUEUE_ITEM_MAX 64
#define TEST_MAX_HANDLERS 8
#define TEST_MAX_SEMS 4

enum task_state {
    TASK_IDLE = 0,   // 已创建、尚未启动线程
    TASK_RUNNING,    // 任务线程正在执行
    TASK_PARKED,     // 任务线程停在一次队列等待,等待测试命令
    TASK_FINISHED,   // 任务线程已自删退出
};

struct test_queue {
    unsigned depth;
    unsigned item_size;
    unsigned count;
    uint8_t items[TEST_QUEUE_MAX_DEPTH][TEST_QUEUE_ITEM_MAX];
};

struct test_task {
    TaskFunction_t entry;
    void *arg;
};

struct test_netif {
    int unused;
};

// 互斥量桩:真实线程锁(测试中用于 netif 发布/读取/销毁互斥)。
// 槽位可回收:启动失败回滚会删除锁,重试再创建;测试据此断言资源不增长。
struct test_sem {
    bool in_use;
    mtx_t lock;
};

struct handler_reg {
    bool live;         // 节点仍在事件循环处理器链表中(已标记也算)
    bool marked;       // 延迟注销已标记:回调禁用,节点保留
    uintptr_t inst_id; // 实例 id(单调递增,避免槽位复用导致指针别名)
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t cb;
    void *arg;
};

static mtx_t s_lock;
static cnd_t s_cv;
static bool s_runtime_ready;

static struct test_queue s_queue;
static bool s_queue_live;
static struct test_task s_task;
static struct test_netif s_netif;
static struct test_sem s_sems[TEST_MAX_SEMS];
static unsigned s_sem_count;  // 槽位高水位(测试用:不得随失败重试增长)
static unsigned s_sem_live;
static struct handler_reg s_handlers[TEST_MAX_HANDLERS];
static unsigned s_handler_count;
static unsigned s_handler_live;
static uintptr_t s_handler_next_inst;  // 实例 id 计数(不随槽位复用)
static bool s_unreg_fault_wifi;  // 模拟"事件循环忙 + 清理事件分配失败"
static bool s_unreg_fault_ip;
static bool s_event_loop_live;

// ---- 默认 legacy STA handlers 模型(ESP-IDF 5.5.3 语义) ----
//
// SDK set_default_wifi_handlers() 用 legacy API(按函数指针匹配)注册
// STA_START/STOP/CONNECTED/DISCONNECTED 与 GOT_IP;SOFTAP 打开时还会注册
// AP_START/AP_STOP——桩只建模与本模块相关的 STA 集合,语义一致。
// clear_default_wifi_handlers() 逐个 legacy 注销并忽略全部返回值。
// 桩复现的语义(锁竞争与分配失败可独立注入,见各注入接口):
//   - 已登记集合再次 set:返回 ESP_OK,不重复注册(静态标志);
//   - legacy 注册同函数:匹配既有节点(含已禁用残点)时仅覆盖参数,
//     不新增节点、不重新启用;
//   - 锁空闲注销:按函数摘除节点,对已禁用残点同样生效(该路径不分配);
//   - 锁忙 + 未标记节点:先标记禁用,再分配清理事件上下文;分配失败
//     (单次注入)则没有清理事件、禁用节点保留;分配成功则清理事件已投递
//     (由事件循环任务摘除,桩在本次清理返回前建模摘除);
//   - 锁忙 + 已标记节点:直接返回 ESP_OK,不摘除、不分配、不投递;
//   - 事件派发跳过已禁用节点。
// 因此"无清理事件的禁用残点"只由分配失败产生(与复审复现一致),它在
// 锁空闲清理前一直保留。
enum {
    k_def_sta_start = 0,
    k_def_sta_stop,
    k_def_sta_connected,
    k_def_sta_disconnected,
    k_def_got_ip,
    k_def_count,
};

static const struct {
    int32_t id;
    bool ip_base;
} k_default_events[k_def_count] = {
    [k_def_sta_start] = { WIFI_EVENT_STA_START, false },
    [k_def_sta_stop] = { WIFI_EVENT_STA_STOP, false },
    [k_def_sta_connected] = { WIFI_EVENT_STA_CONNECTED, false },
    [k_def_sta_disconnected] = { WIFI_EVENT_STA_DISCONNECTED, false },
    [k_def_got_ip] = { IP_EVENT_STA_GOT_IP, true },
};

static struct {
    bool live;
    bool marked;
} s_defaults[k_def_count];
static bool s_defaults_registered;        // SDK:wifi_default_handlers_set
static bool s_defaults_netif_registered;  // SDK:s_wifi_netifs[WIFI_IF_STA]
static bool s_defaults_clear_fault;       // 单次:STA_START 锁忙 + 分配失败
static bool s_defaults_clear_busy;        // 持续:所有 legacy 注销走锁忙分支
static thrd_t s_thread;
static enum task_state s_state = TASK_IDLE;
static bool s_task_created, s_task_finished, s_joined;
static bool s_in_task;
static bool s_resume_event;
static unsigned long s_wait_ms;

static conn_observed_t s_obs;
static int s_fail_stage;

static bool s_creds_exist;
static suite_wifi_cred_t s_creds;
static uint32_t s_ip_addr;
static esp_sntp_sync_cb_t s_sntp_cb;
static wifi_config_t s_last_wifi_cfg;
static int s_last_rssi = -55;
static void (*s_get_ip_info_hook)(void);

// 单次失败注入(定义在 esp_netif/esp_wifi 桩区,提前声明供启动阶段使用)。
static bool stage_failed(int stage);

static void runtime_init(void)
{
    if (s_runtime_ready) return;
    assert(mtx_init(&s_lock, mtx_plain) == thrd_success);
    assert(cnd_init(&s_cv) == thrd_success);
    s_runtime_ready = true;
}

static void test_lock(void)
{
    runtime_init();
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

// ---- 线程化任务与队列 ----

static int task_trampoline(void *arg)
{
    struct test_task *t = arg;
    test_lock();
    s_in_task = true;
    s_state = TASK_RUNNING;
    test_unlock();

    t->entry(t->arg);
    assert(!"conn task returned instead of deleting itself");
    return 0;
}

// 任务线程:记录本次等待时长后停靠,由测试决定恢复方式。
static void task_park(unsigned long wait_ms)
{
    assert(s_in_task);
    s_wait_ms = wait_ms;
    s_state = TASK_PARKED;
    cnd_broadcast(&s_cv);
    while (s_state == TASK_PARKED) cnd_wait(&s_cv, &s_lock);
}

QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size)
{
    test_lock();
    assert(depth <= TEST_QUEUE_MAX_DEPTH && item_size <= TEST_QUEUE_ITEM_MAX);
    if (stage_failed(CONN_FAIL_QUEUE_CREATE)) {
        s_obs.queue_create_fail++;
        test_unlock();
        return NULL;
    }
    assert(!s_task_created || s_task_finished);
    assert(!s_queue_live);
    s_queue.depth = depth;
    s_queue.item_size = item_size;
    s_queue.count = 0;
    s_queue_live = true;
    s_obs.queue_create++;
    test_unlock();
    return &s_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    test_lock();
    assert(queue == &s_queue && item);
    assert(s_queue.count < s_queue.depth);
    memcpy(s_queue.items[s_queue.count], item, s_queue.item_size);
    s_queue.count++;
    test_unlock();
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    test_lock();
    assert(queue == &s_queue && s_queue_live);
    s_queue = (struct test_queue){ 0 };
    s_queue_live = false;
    s_obs.queue_delete++;
    test_unlock();
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t ticks)
{
    test_lock();
    assert(queue == &s_queue && out);
    if (s_queue.count == 0 && ticks != 0) {
        assert(s_in_task);
        task_park((unsigned long)ticks);
        if (!s_resume_event) {
            // 退避超时:队列必须仍为空。
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
    memcpy(out, s_queue.items[0], s_queue.item_size);
    s_queue.count--;
    if (s_queue.count > 0) {
        memmove(s_queue.items[0], s_queue.items[1],
                (size_t)s_queue.count * s_queue.item_size);
    }
    test_unlock();
    return pdTRUE;
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority;
    test_lock();
    assert(entry && task && !s_task_created);
    if (stage_failed(CONN_FAIL_TASK_CREATE)) {
        s_obs.task_create_fail++;
        test_unlock();
        return pdFALSE;
    }
    s_task = (struct test_task){ .entry = entry, .arg = arg };
    s_task_created = true;
    s_state = TASK_IDLE;
    *task = &s_task;
    test_unlock();
    // 真实线程:任务体立即开始运行,停在第一次队列等待时由测试放行。
    assert(thrd_create(&s_thread, task_trampoline, &s_task) == thrd_success);
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL && s_in_task);  // 任务只允许自删
    test_lock();
    s_task_finished = true;
    s_state = TASK_FINISHED;
    cnd_broadcast(&s_cv);
    test_unlock();
    thrd_exit(0);
}

TickType_t xTaskGetTickCount(void)
{
    return 0;  // 固定节拍:等待期限断言与真实时间无关
}

// 生产在"注销确认两轮均 OK"的罕见路径上让出一次,给事件循环任务释放
// 已投递清理事件的机会;桩不模拟异步任务,直接返回。
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

// ---- 互斥量桩 ----

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static const size_t k_max_slots = TEST_MAX_SEMS;
    test_lock();
    if (stage_failed(CONN_FAIL_LOCK_CREATE)) {
        s_obs.mutex_create_fail++;
        test_unlock();
        return NULL;
    }
    struct test_sem *slot = NULL;
    for (size_t i = 0; i < k_max_slots; i++) {
        if (!s_sems[i].in_use) {
            slot = &s_sems[i];
            if (i + 1 > s_sem_count) s_sem_count = (unsigned)(i + 1);
            break;
        }
    }
    if (!slot) {  // 槽位耗尽:生产按 ESP_ERR_NO_MEM 处理
        test_unlock();
        return NULL;
    }
    slot->in_use = true;
    assert(mtx_init(&slot->lock, mtx_plain) == thrd_success);
    s_sem_live++;
    s_obs.mutex_create++;
    test_unlock();
    return slot;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;  // 生产代码只用 portMAX_DELAY
    assert(sem && sem->in_use);
    return mtx_lock(&sem->lock) == thrd_success ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem && sem->in_use);
    int rc = mtx_unlock(&sem->lock);
    assert(rc == thrd_success);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    test_lock();
    assert(sem && sem->in_use && s_sem_live > 0);
    mtx_destroy(&sem->lock);
    sem->in_use = false;
    s_sem_live--;
    s_obs.mutex_delete++;
    test_unlock();
}

// ---- 事件循环与事件处理器 ----

esp_err_t esp_event_loop_create_default(void)
{
    test_lock();
    if (s_event_loop_live) {
        // 已存在:真实 esp_event 返回 ESP_ERR_INVALID_STATE(幂等容忍)。
        s_obs.event_loop_existing++;
        test_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (stage_failed(CONN_FAIL_EVENT_LOOP)) {
        s_obs.event_loop_fail++;
        test_unlock();
        return ESP_FAIL;
    }
    s_event_loop_live = true;
    s_obs.event_loop_created++;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t callback,
                                              void *arg,
                                              esp_event_handler_instance_t *instance)
{
    test_lock();
    assert(callback && s_handler_count < TEST_MAX_HANDLERS);
    // 先比较 base 再消费注入:否则不相干的注册调用会提前消费掉注入。
    bool inject = false;
    if (strcmp(base, WIFI_EVENT) == 0) {
        inject = stage_failed(CONN_FAIL_HANDLER_WIFI);
    } else if (strcmp(base, IP_EVENT) == 0) {
        inject = stage_failed(CONN_FAIL_HANDLER_IP);
    }
    if (inject) {
        s_obs.handler_reg_fail++;
        if (instance) *instance = NULL;
        test_unlock();
        return ESP_FAIL;
    }
    // 重复注册检测:同一 (base, id, callback) 再次注册即为重复。
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].live && !s_handlers[i].marked &&
            s_handlers[i].id == id && s_handlers[i].cb == callback &&
            strcmp(s_handlers[i].base, base) == 0) {
            s_obs.handler_duplicate++;
        }
    }
    // 槽位复用:被摘除的槽位可以重新注册(生产在失败回滚后会重试注册)。
    unsigned slot = s_handler_count;
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (!s_handlers[i].live) {
            slot = i;
            break;
        }
    }
    if (slot == s_handler_count) {
        assert(s_handler_count < TEST_MAX_HANDLERS);
        s_handler_count++;
    }
    s_handlers[slot] = (struct handler_reg){
        .live = true,
        .marked = false,
        .inst_id = ++s_handler_next_inst,
        .base = base,
        .id = id,
        .cb = callback,
        .arg = arg,
    };
    s_handler_live++;
    s_obs.handler_regs++;
    // 实例句柄:单调递增的 id(槽位复用不会让旧句柄指向新节点)。
    if (instance) *instance = (void *)(uintptr_t)s_handlers[slot].inst_id;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance)
{
    test_lock();
    uintptr_t inst_id = (uintptr_t)instance;
    if (inst_id == 0) {
        s_obs.handler_unreg_missing++;
        test_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    struct handler_reg *h = NULL;
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].live && s_handlers[i].inst_id == inst_id) {
            h = &s_handlers[i];
            break;
        }
    }
    // 节点不在 (base, id) 子树中、或已被摘除:SDK 返回 ESP_ERR_NOT_FOUND
    // (确认协议的第二次调用走这里;仅做 id 比较,不解引用已释放句柄)。
    if (!h || strcmp(h->base, base) != 0 || h->id != id) {
        test_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    bool fault = (strcmp(h->base, WIFI_EVENT) == 0) ? s_unreg_fault_wifi
                                                    : s_unreg_fault_ip;
    if (fault) {
        if (!h->marked) {
            // 锁忙 + 清理事件分配失败:节点被标记(回调禁用)但保留,
            // 返回 ESP_ERR_NO_MEM(与 esp_event_post_to 的 calloc 失败一致)。
            h->marked = true;
            s_obs.handler_unreg_marked++;
            test_unlock();
            return ESP_ERR_NO_MEM;
        }
        // 已标记节点的再次注销:锁忙路径返回 OK 但不摘除(空操作)。
        test_unlock();
        return ESP_OK;
    }
    // 锁空闲:立即摘除并释放。
    h->live = false;
    h->marked = false;
    s_handler_live--;
    s_obs.handler_unregs++;
    test_unlock();
    return ESP_OK;
}

static bool stage_failed(int stage)
{
    // 注入只触发一次,重试路径可再次注入。
    if (s_fail_stage != stage) return false;
    s_fail_stage = CONN_FAIL_NONE;
    return true;
}

// ---- esp_netif / esp_wifi ----

esp_err_t esp_netif_init(void)
{
    test_lock();
    s_obs.netif_init_calls++;
    if (stage_failed(CONN_FAIL_NETIF_INIT)) {
        s_obs.netif_init_fail++;
        test_unlock();
        return ESP_FAIL;
    }
    test_unlock();
    return ESP_OK;
}

esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg)
{
    (void)cfg;
    test_lock();
    assert(!s_obs.netif_live && !s_obs.driver_live);
    if (stage_failed(CONN_FAIL_NETIF_NEW)) {
        s_obs.netif_new_fail++;
        test_unlock();
        return NULL;
    }
    s_obs.netif_new++;
    s_obs.netif_live = true;
    test_unlock();
    return &s_netif;
}

esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
    test_lock();
    assert(netif == &s_netif && s_obs.netif_live && !s_obs.driver_live);
    s_obs.netif_attach++;
    // SDK 先登记默认 netif(wifi_default.c 的 s_wifi_netifs),再创建并挂接
    // 驱动;attach 失败时登记仍保留,由 destroy_default_wifi 一并撤销。
    s_defaults_netif_registered = true;
    s_obs.driver_live = true;
    if (stage_failed(CONN_FAIL_NETIF_ATTACH)) {
        s_obs.netif_attach_fail++;
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    test_unlock();
    return ESP_OK;
}

// clear_default_wifi_handlers():逐个 legacy 注销,SDK 忽略全部返回值。
// 锁竞争(s_defaults_clear_busy,持续)与清理上下文分配失败
// (s_defaults_clear_fault,单次、只打在 STA_START)独立注入:
//   - 锁空闲:立即摘除(包括已禁用残点),不分配;
//   - 锁忙 + 未标记:标记后分配清理上下文,失败 → 无清理事件、禁用残点
//     保留;成功 → 清理事件已投递(事件循环任务摘除,本次返回前建模完成);
//   - 锁忙 + 已标记:返回 OK 但不摘除、不分配、不投递。
static void defaults_clear_locked(void)
{
    s_obs.defaults_clear_calls++;
    bool cleanup_posted[k_def_count] = { false };
    for (unsigned i = 0; i < k_def_count; i++) {
        if (!s_defaults[i].live) continue;
        bool busy = s_defaults_clear_busy ||
                    (s_defaults_clear_fault && i == k_def_sta_start);
        if (!busy) {
            s_defaults[i].live = false;
            s_defaults[i].marked = false;
            s_obs.defaults_freed++;
            continue;
        }
        if (s_defaults[i].marked) {
            // 已标记节点再次注销:锁忙空操作(返回 OK,不摘除/不分配/不投递)。
            s_obs.defaults_marked_busy_ok++;
            continue;
        }
        // 未标记节点:先标记(回调禁用),再分配清理事件上下文。
        s_defaults[i].marked = true;
        if (s_defaults_clear_fault && i == k_def_sta_start) {
            s_defaults_clear_fault = false;  // 单次分配失败:后续注销恢复
            s_obs.defaults_clear_alloc_fail++;  // 无清理事件:禁用残点保留
            continue;
        }
        s_obs.defaults_mark_cleanup++;
        cleanup_posted[i] = true;
    }
    // 已投递的清理事件由事件循环任务在本次注销返回后处理(调用方是另一
    // 个任务,事件循环任务并发运行);桩在此处建模为"本次清理内摘除"。
    for (unsigned i = 0; i < k_def_count; i++) {
        if (!cleanup_posted[i]) continue;
        s_defaults[i].live = false;
        s_defaults[i].marked = false;
        s_obs.defaults_freed++;
    }
    s_defaults_registered = false;
    s_obs.defaults_live = false;
}

esp_err_t esp_wifi_set_default_wifi_sta_handlers(void)
{
    test_lock();
    s_obs.defaults_set++;
    // SDK:静态标志已置位时直接返回 ESP_OK(不重复注册)。
    if (s_defaults_registered) {
        test_unlock();
        return ESP_OK;
    }
    // 部分注册失败:第 3 个注册失败;SDK 的 fail 路径调用
    // clear_default_wifi_handlers() 释放已注册的处理器(仍可能因锁忙 +
    // 分配失败留下禁用残点)。
    bool partial_fail = stage_failed(CONN_FAIL_DEFAULT_HANDLERS);
    for (unsigned i = 0; i < k_def_count; i++) {
        if (partial_fail && i == 2) break;
        if (s_defaults[i].live) {
            // legacy 同函数再注册:仅覆盖参数,不新增节点、不重新启用。
            s_obs.defaults_dup_regs++;
            continue;
        }
        s_defaults[i].live = true;
        s_defaults[i].marked = false;
        s_obs.defaults_regs++;
    }
    if (partial_fail) {
        s_obs.defaults_fail++;
        defaults_clear_locked();  // SDK fail 路径:clear_default_wifi_handlers()
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_defaults_registered = true;
    s_obs.defaults_live = true;
    test_unlock();
    return ESP_OK;
}

void esp_netif_destroy_default_wifi(esp_netif_t *netif)
{
    test_lock();
    assert(netif == &s_netif && s_obs.netif_live);
    assert(!s_obs.wifi_live);  // 必须先反初始化 Wi-Fi 再销毁 netif
    s_obs.netif_destroy++;
    s_obs.netif_live = false;
    s_obs.driver_live = false;
    // SDK:esp_wifi_clear_default_wifi_driver_and_handlers() → 默认 netif
    // 全部注销后 clear_default_wifi_handlers();随后销毁 netif。
    s_defaults_netif_registered = false;
    defaults_clear_locked();
    test_unlock();
}

// 已禁用残点的有界清理入口(生产在 attach 之前调用):SDK 只有在默认
// netif 全部注销时才真正清理;传入刚创建、尚未 attach 的新 netif 时
// s_wifi_netifs 为空,命中清理;disconnect_and_destroy 部分为空操作。
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(void *esp_netif)
{
    test_lock();
    assert(esp_netif == &s_netif);
    s_obs.defaults_sweep_calls++;
    if (!s_defaults_netif_registered) {
        defaults_clear_locked();
    }
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg)
{
    (void)cfg;
    test_lock();
    assert(s_obs.defaults_live && !s_obs.wifi_live && !s_obs.wifi_running);
    s_obs.wifi_init++;
    if (stage_failed(CONN_FAIL_WIFI_INIT)) {
        s_obs.wifi_init_fail++;
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_obs.wifi_live = true;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    test_lock();
    assert(s_obs.wifi_live && !s_obs.wifi_running);
    s_obs.wifi_deinit++;
    s_obs.wifi_live = false;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(int storage)
{
    (void)storage;
    test_lock();
    assert(s_obs.wifi_live);
    if (stage_failed(CONN_FAIL_WIFI_STORAGE)) {
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(int mode)
{
    (void)mode;
    test_lock();
    assert(s_obs.wifi_live);
    if (stage_failed(CONN_FAIL_WIFI_MODE)) {
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    test_lock();
    assert(s_obs.wifi_live && !s_obs.wifi_running);
    s_obs.wifi_start++;
    if (stage_failed(CONN_FAIL_WIFI_START)) {
        s_obs.wifi_start_fail++;
        test_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_obs.wifi_running = true;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    test_lock();
    assert(s_obs.wifi_running);
    s_obs.wifi_stop++;
    s_obs.wifi_running = false;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_disconnect(void)
{
    test_lock();
    assert(s_obs.wifi_running);
    s_obs.wifi_disconnect++;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    test_lock();
    assert(s_obs.wifi_running);
    s_obs.wifi_connect++;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(int interface, wifi_config_t *conf)
{
    (void)interface;
    test_lock();
    assert(s_obs.wifi_running && conf);
    s_last_wifi_cfg = *conf;
    s_obs.wifi_set_config++;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    test_lock();
    assert(s_obs.wifi_running && ap);
    ap->rssi = s_last_rssi;
    test_unlock();
    return ESP_OK;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    // 读者侧交错钩子:在读者持有生命周期锁、尚未完成读取时调用,
    // 便于测试观察"销毁必须等待读者"的顺序。
    test_lock();
    void (*hook)(void) = s_get_ip_info_hook;
    test_unlock();
    if (hook) hook();

    test_lock();
    assert(netif == &s_netif && s_obs.netif_live && info);
    info->ip.addr = s_ip_addr;
    info->netmask.addr = 0x00FFFFFFu;
    info->gw.addr = 0x0101A8C0u;
    test_unlock();
    return ESP_OK;
}

// ---- esp_sntp ----

void esp_sntp_setoperatingmode(int mode) { (void)mode; }
void esp_sntp_setservername(unsigned idx, const char *host)
{
    (void)idx;
    (void)host;
}
void esp_sntp_set_time_sync_notification_cb(esp_sntp_sync_cb_t cb) { s_sntp_cb = cb; }
void esp_sntp_init(void) { s_obs.sntp_init++; }
void esp_sntp_stop(void) { s_obs.sntp_stop++; }

// ---- NVS 凭据桩 ----

esp_err_t suite_storage_init(void) { return ESP_OK; }

bool suite_storage_wifi_exists(void)
{
    test_lock();
    bool exists = s_creds_exist;
    test_unlock();
    return exists;
}

esp_err_t suite_storage_wifi_get(suite_wifi_cred_t *out)
{
    test_lock();
    if (!s_creds_exist) {
        test_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_creds;
    test_unlock();
    return ESP_OK;
}

esp_err_t suite_storage_wifi_set(const suite_wifi_cred_t *in)
{
    test_lock();
    s_creds = *in;
    s_creds_exist = true;
    test_unlock();
    return ESP_OK;
}

// ---- 测试控制面 ----

const conn_observed_t *test_conn_observed(void) { return &s_obs; }
unsigned test_conn_fail_stage(void) { return (unsigned)s_fail_stage; }

// 本场景内新建且仍存活的资源(启动失败回滚后必须回到 0)。
unsigned test_conn_live_queues(void)
{
    test_lock();
    unsigned live = s_queue_live ? 1u : 0u;
    test_unlock();
    return live;
}

unsigned test_conn_live_mutexes(void)
{
    test_lock();
    unsigned live = s_sem_live;
    test_unlock();
    return live;
}

unsigned test_conn_live_handlers(void)
{
    test_lock();
    unsigned live = s_handler_live;
    test_unlock();
    return live;
}

// 已被标记但尚未释放的处理器节点数(仍计入 live;回调已禁用)。
unsigned test_conn_marked_handlers(void)
{
    test_lock();
    unsigned marked = 0;
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].live && s_handlers[i].marked) marked++;
    }
    test_unlock();
    return marked;
}

// 默认 legacy handlers:节点数(含已禁用残点)与残点数。
unsigned test_conn_defaults_live(void)
{
    test_lock();
    unsigned live = 0;
    for (unsigned i = 0; i < k_def_count; i++) {
        if (s_defaults[i].live) live++;
    }
    test_unlock();
    return live;
}

unsigned test_conn_defaults_marked(void)
{
    test_lock();
    unsigned marked = 0;
    for (unsigned i = 0; i < k_def_count; i++) {
        if (s_defaults[i].live && s_defaults[i].marked) marked++;
    }
    test_unlock();
    return marked;
}

// 默认 handlers 清理故障注入(单次,自动解除):下一次默认清理中
// STA_START 的 legacy 注销走"锁忙 + 清理上下文分配失败"路径。
void test_conn_set_defaults_clear_fault(bool on)
{
    test_lock();
    s_defaults_clear_fault = on;
    test_unlock();
}

// 默认 handlers 清理的锁竞争注入(持续):开启后所有 legacy 注销走锁忙
// 分支——未标记节点被标记(分配成功则清理事件已投递,桩在锁空闲清理时
// 摘除),已标记节点返回 OK 但不摘除;关闭后锁空闲清理立即摘除残点。
void test_conn_set_defaults_clear_busy(bool on)
{
    test_lock();
    s_defaults_clear_busy = on;
    test_unlock();
}

// 延迟注销语义注入(ESP-IDF 5.5.3 esp_event,已按源码核对):
// 事件循环锁忙时,注销先把节点标记为已注销(回调禁用)再分配清理事件;
// 分配失败返回 ESP_ERR_NO_MEM 且节点保留。开启注入后,对该 base 的注销
// 走上述路径:未标记节点 → 标记并返回 ESP_ERR_NO_MEM;已标记节点 →
// 返回 ESP_OK 但不摘除(锁忙空操作)。关闭注入后按锁空闲路径立即摘除。
void test_conn_set_unreg_fault(esp_event_base_t base, bool on)
{
    test_lock();
    if (strcmp(base, WIFI_EVENT) == 0) {
        s_unreg_fault_wifi = on;
    } else if (strcmp(base, IP_EVENT) == 0) {
        s_unreg_fault_ip = on;
    } else {
        assert(!"unknown event base");
    }
    test_unlock();
}

// 默认事件循环是进程级共享单例:启动失败不回收,重试按"已存在"容忍。
bool test_conn_event_loop_live(void)
{
    test_lock();
    bool live = s_event_loop_live;
    test_unlock();
    return live;
}

void test_conn_reset(void)
{
    test_lock();
    assert(s_state == TASK_IDLE || s_state == TASK_FINISHED);
    assert(!s_obs.netif_live && !s_obs.driver_live && !s_obs.defaults_live);
    assert(!s_obs.wifi_live && !s_obs.wifi_running);
    s_state = TASK_IDLE;
    s_task_created = false;
    s_task_finished = false;
    s_joined = false;
    s_in_task = false;
    s_resume_event = false;
    s_wait_ms = 0;
    s_queue = (struct test_queue){ 0 };
    s_queue_live = false;
    memset(s_sems, 0, sizeof(s_sems));
    s_sem_count = 0;
    s_sem_live = 0;
    s_get_ip_info_hook = NULL;
    s_obs = (conn_observed_t){ 0 };
    s_fail_stage = CONN_FAIL_NONE;
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_count = 0;
    s_handler_live = 0;
    s_handler_next_inst = 0;
    s_unreg_fault_wifi = false;
    s_unreg_fault_ip = false;
    memset(s_defaults, 0, sizeof(s_defaults));
    s_defaults_registered = false;
    s_defaults_netif_registered = false;
    s_defaults_clear_fault = false;
    s_defaults_clear_busy = false;
    s_event_loop_live = false;  // 场景之间复位为"全新进程"
    s_sntp_cb = NULL;
    s_last_wifi_cfg = (wifi_config_t){ 0 };
    s_ip_addr = 0;
    test_unlock();
}

void test_conn_fail_at(int stage)
{
    test_lock();
    assert(stage >= CONN_FAIL_NONE && stage <= CONN_FAIL_WIFI_START);
    s_fail_stage = stage;
    test_unlock();
}

void test_conn_set_creds(const char *ssid, const char *pass)
{
    test_lock();
    memset(&s_creds, 0, sizeof(s_creds));
    strlcpy(s_creds.ssid, ssid, sizeof(s_creds.ssid));
    strlcpy(s_creds.password, pass, sizeof(s_creds.password));
    s_creds_exist = true;
    test_unlock();
}

void test_conn_clear_creds(void)
{
    test_lock();
    s_creds_exist = false;
    test_unlock();
}

void test_conn_set_ip(uint32_t addr)
{
    test_lock();
    s_ip_addr = addr;
    test_unlock();
}

// 默认 legacy handlers:实例处理器之后按注册顺序派发;已禁用(已标记)
// 节点跳过(SDK 的 unregistered 语义)。按具体 id 注册。
static void defaults_dispatch_locked(int32_t id)
{
    for (unsigned i = 0; i < k_def_count; i++) {
        if (k_default_events[i].ip_base) continue;
        if (!s_defaults[i].live || s_defaults[i].marked) continue;
        if (k_default_events[i].id != id) continue;
        s_obs.defaults_invocations++;
        if (id == WIFI_EVENT_STA_START) {
            s_obs.defaults_netif_start++;   // 默认动作:启动 netif
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            s_obs.defaults_dhcp_start++;    // 默认动作:连接后启动 DHCP
        }
    }
}

void test_conn_default_dispatch(int32_t id)
{
    defaults_dispatch_locked(id);
}

void test_conn_wifi_event(int32_t id, void *data)
{
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].live && !s_handlers[i].marked &&
            strcmp(s_handlers[i].base, WIFI_EVENT) == 0 &&
            (s_handlers[i].id == ESP_EVENT_ANY_ID ||
             s_handlers[i].id == id)) {
            s_obs.handler_invocations++;
            s_handlers[i].cb(s_handlers[i].arg, WIFI_EVENT, id, data);
        }
    }
    defaults_dispatch_locked(id);
}

void test_conn_ip_got_ip(void)
{
    for (unsigned i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].live && !s_handlers[i].marked &&
            strcmp(s_handlers[i].base, IP_EVENT) == 0 &&
            s_handlers[i].id == IP_EVENT_STA_GOT_IP) {
            s_obs.handler_invocations++;
            s_handlers[i].cb(s_handlers[i].arg, IP_EVENT, IP_EVENT_STA_GOT_IP,
                             NULL);
        }
    }
    for (unsigned i = 0; i < k_def_count; i++) {
        if (!k_default_events[i].ip_base) continue;
        if (!s_defaults[i].live || s_defaults[i].marked) continue;
        s_obs.defaults_invocations++;
        s_obs.defaults_got_ip++;
    }
}

void test_conn_sntp_fire(int64_t epoch)
{
    if (!s_sntp_cb) return;
    struct timeval tv = { .tv_sec = (time_t)epoch };
    s_sntp_cb(&tv);
}

unsigned long test_conn_wait_park(void)
{
    test_lock();
    while (s_state == TASK_IDLE || s_state == TASK_RUNNING) {
        cnd_wait(&s_cv, &s_lock);
    }
    unsigned long wait = 0;
    if (s_state == TASK_FINISHED) {
        if (!s_joined) {
            s_joined = true;
            test_unlock();
            assert(thrd_join(s_thread, NULL) == thrd_success);
            test_lock();
        }
    } else {
        assert(s_state == TASK_PARKED);
        wait = s_wait_ms;
    }
    test_unlock();
    return wait;
}

void test_conn_resume_timeout(void)
{
    test_lock();
    assert(s_state == TASK_PARKED);
    assert(s_queue.count == 0);
    s_resume_event = false;
    s_state = TASK_RUNNING;
    cnd_broadcast(&s_cv);
    test_unlock();
}

void test_conn_resume_event(void)
{
    test_lock();
    assert(s_state == TASK_PARKED);
    assert(s_queue.count > 0);
    s_resume_event = true;
    s_state = TASK_RUNNING;
    cnd_broadcast(&s_cv);
    test_unlock();
}

bool test_conn_finished(void)
{
    test_lock();
    bool finished = s_task_finished;
    test_unlock();
    return finished;
}

void test_conn_set_get_ip_info_hook(void (*hook)(void))
{
    test_lock();
    s_get_ip_info_hook = hook;
    test_unlock();
}

bool test_conn_wait_deinit(unsigned long timeout_ms)
{
    for (unsigned long waited = 0;; waited += 5) {
        test_lock();
        bool done = s_obs.wifi_deinit > 0;
        test_unlock();
        if (done) return true;
        if (waited >= timeout_ms) return false;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000L };
        thrd_sleep(&ts, NULL);
    }
}

const char *test_conn_last_ssid(void) { return s_last_wifi_cfg.sta.ssid; }
const char *test_conn_last_pass(void) { return s_last_wifi_cfg.sta.password; }
