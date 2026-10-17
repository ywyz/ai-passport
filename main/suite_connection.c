// main/suite_connection.c —— Wi-Fi STA + SNTP 设备侧实现。
#include "suite_connection.h"
#include "suite_storage.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <string.h>
#include <time.h>

static const char *TAG = "suite_conn";

#define CONN_QUEUE_DEPTH 8
#define CONN_TASK_STACK 4096
#define CONN_NTP_SERVERS 3
// 默认处理器残点的单次有界清理尝试次数(每次尝试后让出一次任务给事件
// 循环任务释放锁;尝试次数不是恢复依据,见 s_default_handlers_sweep_pending)。
#define CONN_DEFAULT_SWEEP_ATTEMPTS 3
// WAIT_IP 硬超时:已关联却迟迟没有 GOT_IP 时按失败处理。本固件只有默认
// STA_CONNECTED 处理器会注册收包回调并启动 DHCP,该超时覆盖"默认处理器
// 残点导致连接恢复卡住"的场景;超时后先进入退避(保留网络栈,迟到的
// GOT_IP 仍可恢复在线),退避到期由重试路径整栈重建,在 attach 前重新
// 执行有界残点清理。清理未验证期间(尚未拿到 GOT_IP)的任何退避重试
// 都直接整栈重建(见 conn_schedule),该超时只是"DHCP 一直无结果"的
// 兜底。取值 30s 远高于正常关联+DHCP(<5s),同时保证不会在 WAIT_IP
// 无限等待。
#define CONN_IP_WAIT_TIMEOUT_MS (30u * 1000u)

typedef struct {
    suite_conn_machine_t fsm;
    QueueHandle_t queue;
    TaskHandle_t task;
    esp_netif_t *netif;          // 唯一所有者是 conn_task;见 netif 生命周期说明
    SemaphoreHandle_t netif_lock;  // netif 发布/读取/销毁互斥(任务上下文)
    esp_event_handler_instance_t wifi_handler;  // 本模块注册的 WIFI_EVENT 实例
    esp_event_handler_instance_t ip_handler;    // 本模块注册的 IP_EVENT 实例
    suite_time_tracker_t time;
    portMUX_TYPE time_mux;
    volatile suite_conn_state_t state_pub;
    volatile bool sntp_up;
    bool wifi_inited;
    bool wifi_started;
    bool running;
    uint8_t ignore_lost;  // 应用新凭据期间需要吞掉的旧链路迟到 DISCONNECTED 数
    bool rebuild_on_retry;  // WAIT_IP 超时或清理未验证:退避到期后整栈重建
    TickType_t retry_deadline;  // RETRY_WAIT 的退避截止时刻
    TickType_t ip_deadline;     // WAIT_IP 的等待 IP 截止时刻(0=未设置)
} conn_ctx_t;

static conn_ctx_t s_conn;

static const char *const k_ntp_hosts[CONN_NTP_SERVERS] = {
    "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov",
};

// esp_event 回调 → 事件队列(满则丢弃并计数,保证回调轻量)。
enum {
    Q_WIFI_STARTED = SUITE_CONN_EV_WIFI_STARTED,
    Q_ASSOC_FAIL = SUITE_CONN_EV_ASSOC_FAIL,
    Q_GOT_IP = SUITE_CONN_EV_GOT_IP,
    Q_LOST = SUITE_CONN_EV_LOST,
    Q_STOP = SUITE_CONN_EV_STOP,
    Q_APPLY_NEW = SUITE_CONN_EV_APPLY_NEW,
    Q_RETRY_NOW = 100,   // 显式取值,避免与隐式续排的事件值冲突
};

typedef struct {
    uint8_t type;
    uint16_t reason;     // Q_LOST 时携带 wifi disconnect reason
} queue_ev_t;

static void post_ev(uint8_t type, uint16_t reason)
{
    if (!s_conn.queue) return;
    queue_ev_t ev = { .type = type, .reason = reason };
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

// 完整收栈(定义在 wifi_stack_up 之后)。
static void wifi_stack_down(void);

// netif 所有权与生命周期(唯一所有者 = conn_task):
//   创建(esp_netif_new → attach → 默认事件处理器)→ 发布 → 读取 → 销毁。
//   发布、读取(读者如配网/UI 任务调用 suite_connection_ip_str)与销毁
//   共享 netif_lock:读者持锁完成"取指针 + 查询 IP",销毁者在同一把锁
//   内 destroy + 置 NULL,因此不存在"读者取得指针后对象被释放"的交错;
//   仅提前置 NULL 保护不了已经拿到指针的读者。
//   锁只在任务上下文使用;持锁区间只包含 netif 调用,不在临界区/自旋锁
//   内执行 ESP-IDF 接口。
static void netif_publish(esp_netif_t *nif)
{
    if (s_conn.netif_lock) (void)xSemaphoreTake(s_conn.netif_lock, portMAX_DELAY);
    s_conn.netif = nif;
    if (s_conn.netif_lock) (void)xSemaphoreGive(s_conn.netif_lock);
}

// 默认 STA legacy handlers 的"清理未验证"状态(只在 conn_task 上下文读写):
// SDK 的 esp_wifi_set_default_wifi_sta_handlers() 用 legacy 注册
// (esp_event_handler_register,按函数指针匹配)登记 STA_START/STOP/
// CONNECTED/DISCONNECTED 与 GOT_IP;esp_netif_destroy_default_wifi() 内部
// 走 esp_wifi_clear_default_wifi_driver_and_handlers() →
// clear_default_wifi_handlers()(已按 ESP-IDF 5.5.3 wifi_default.c 核对),
// 逐个 legacy 注销并忽略全部返回值。esp_event 的注销语义(已按
// esp_event.c 核对):
//   - 事件循环锁空闲:loop_remove_handler 按函数指针立即摘除节点(对已
//     标记的节点同样生效)并返回 ESP_OK;
//   - 锁忙:find_and_unregister_handler 先把节点标记 unregistered(回调
//     从此禁用),再分配一条清理事件交给事件循环任务释放节点;该分配
//     失败返回 ESP_ERR_NO_MEM,节点被禁用却不再有清理事件;对已标记
//     节点再次注销是"锁忙空操作"(返回 ESP_OK 却不摘除、不补发清理)。
// 因此销毁 netif 后可能遗留"已禁用但未摘除"的默认节点;下一次
// esp_wifi_set_default_wifi_sta_handlers() 按函数指针匹配到该残点,
// 返回 ESP_OK 但不会重新启用它,默认 STA 启动/连接(含 netif 动作与
// DHCP)会静默不再执行。
// 关键限制:清理入口的返回值不可用作证据——SDK 返回的是
// disconnect_and_destroy 的结果,clear_default_wifi_handlers() 还把内部
// 每个注销的返回值全部丢弃(锁忙空操作也返回 OK)。因此本标志的语义是
// "可能仍有残点且清理未经验证":
//   - 销毁默认 netif 时置位;
//   - 每次创建 netif 时在 attach 之前做一次有界清理尝试(见
//     conn_sweep_default_handlers),但绝不由该调用的返回或次数清零;
//   - 只有在拿到功能证据后才清零:GOT_IP 只可能出现在默认 STA_START
//     处理器启动 netif、默认 STA_CONNECTED 处理器注册收包回调并启动
//     DHCP 之后(本固件没有其它途径启动 DHCP/收包),所以拿到 IP 即
//     证明默认处理器链在本代栈上真实工作(见 conn_task 的 Q_GOT_IP);
//   - WAIT_IP 硬超时(见 conn_schedule/conn_task)保证残点导致默认
//     处理器失效时不会无限等待 IP:超时整体重建网络栈,把清理重试交给
//     下一次 attach 前的清理,而不是把恢复押在未来某次偶然销毁上;
//   - 清理未验证期间进入 RETRY_WAIT 时,退避到期一律整栈重建(见
//     conn_schedule 与 Q_RETRY_NOW):复用网络栈无法重跑 attach 前的
//     清理,也不会再产生 STA_START,仅重新关联不带来任何恢复机会。
//     否则"WAIT_IP 期间先收到 DISCONNECTED"会把等待带出有期限的
//     WAIT_IP,停在无期限的 CONNECTING,残点再也不被清理(复审 P2)。
static bool s_default_handlers_sweep_pending;

// 销毁默认 STA netif,并标记 SDK 对默认 handlers 的清理结果未经验证
// (销毁路径内部的注销可能因事件循环锁忙 + 清理事件分配失败留下已禁用
// 残点;见 s_default_handlers_sweep_pending 的说明)。
static void netif_destroy(esp_netif_t *nif)
{
    esp_netif_destroy_default_wifi(nif);
    s_default_handlers_sweep_pending = true;
}

static void netif_destroy_published(void)
{
    if (s_conn.netif_lock) (void)xSemaphoreTake(s_conn.netif_lock, portMAX_DELAY);
    if (s_conn.netif) {
        netif_destroy(s_conn.netif);
        s_conn.netif = NULL;
    }
    if (s_conn.netif_lock) (void)xSemaphoreGive(s_conn.netif_lock);
}

// 有界尝试摘除默认处理器残点:必须在默认 netif 尚未 attach 时调用——
// SDK 的 esp_wifi_clear_default_wifi_driver_and_handlers() 只有在
// s_wifi_netifs 全部为空时才真正进入 clear_default_wifi_handlers()。
// 传入刚创建、尚未 attach 的 netif 只用于满足接口参数:该 netif 不在
// SDK 默认表中(条件成立),且 driver_handle 为空,disconnect 部分是
// 安全的空操作,不销毁任何对象、不双重释放。
// 语义与边界(ESP-IDF 5.5.3,已核对 wifi_default.c / esp_event.c):
//   - 锁空闲:按函数指针摘除全部默认节点,包括已禁用残点;该路径不分配
//     内存,内存紧张时同样有效;
//   - 锁忙:未标记节点被标记后分配清理事件上下文,失败即留下"无清理
//     事件"的禁用残点;已标记残点走"已标记"分支返回 OK(不分配、不摘除);
//     两者都不新增节点,因此本次清理不会制造新残点;
//   - 返回值与尝试次数都不能证明节点已摘除(见上),所以本函数不清零
//     s_default_handlers_sweep_pending;
//   - 尝试次数固定,每次之间让出一次任务给事件循环任务释放锁(不忙等、
//     不无限重试);本次未能摘除的残点由功能验证失败后的整栈重建兜底。
static void conn_sweep_default_handlers(esp_netif_t *fresh_nif)
{
    if (!s_default_handlers_sweep_pending) return;
    for (int attempt = 1; attempt <= CONN_DEFAULT_SWEEP_ATTEMPTS; attempt++) {
        (void)esp_wifi_clear_default_wifi_driver_and_handlers(fresh_nif);
        if (attempt < CONN_DEFAULT_SWEEP_ATTEMPTS) {
            // 事件循环任务优先级更高:让出一个 tick 足以让它完成当前
            // 派发并释放事件循环锁,下一次尝试就能走锁空闲路径。
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    ESP_LOGW(TAG, "已对默认 Wi-Fi 事件处理器做 %d 次有界清理尝试(结果不可验证,待功能证据)",
             CONN_DEFAULT_SWEEP_ATTEMPTS);
}

// Wi-Fi 栈起步:netif → 默认处理器 → esp_wifi_init → 模式 → start。
// 任一步失败都完整回滚(wifi_stack_down),因此失败不会留下半初始化标志、
// 重复注册或泄漏;退避后的重试从零开始重新初始化。返回 ESP_OK 或首个错误。
static esp_err_t wifi_stack_up(void)
{
    esp_err_t err;

    // esp_netif_init 与默认事件循环已在 suite_connection_start 中创建。
    if (!s_conn.netif) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
        esp_netif_t *nif = esp_netif_new(&cfg);
        if (!nif) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        // attach 之前清理上一次销毁可能遗留的默认处理器残点(有界尝试;
        // 清理未验证前 s_default_handlers_sweep_pending 保持置位,
        // 见 conn_sweep_default_handlers)。
        conn_sweep_default_handlers(nif);
        // attach 失败也会登记 netif;destroy_default_wifi 会一并撤销登记
        // 与部分注册的默认 STA 事件处理器。
        err = esp_netif_attach_wifi_station(nif);
        if (err != ESP_OK) {
            // 尚未发布:读者不可见,直接销毁后走统一回滚。
            netif_destroy(nif);
            goto fail;
        }
        err = esp_wifi_set_default_wifi_sta_handlers();
        if (err != ESP_OK) {
            netif_destroy(nif);
            goto fail;
        }
        // 全部就绪后才发布给读者。
        netif_publish(nif);
    }
    if (!s_conn.wifi_inited) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        // BLE+Wi-Fi 共存下内存紧张,NO_MEM 时保持离线比 abort 重启更有用。
        err = esp_wifi_init(&wcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init 失败(空闲堆 %lu,最大连续内部块 %lu)",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            goto fail;
        }
        s_conn.wifi_inited = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) goto fail;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) goto fail;
    }
    if (!s_conn.wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) goto fail;
        s_conn.wifi_started = true;
    }
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi 栈初始化失败: %s", esp_err_to_name(err));
    wifi_stack_down();
    return err;
}

// 完整收栈:停止连接/驱动、反初始化 Wi-Fi、销毁默认 STA netif
// (同时撤销 attach 登记与默认事件处理器;SDK 注销结果不可验证,由
// s_default_handlers_sweep_pending 记录并在下一次 wifi_stack_up 的
// attach 前重新尝试清理,见 netif_destroy)。可重复调用。
static void wifi_stack_down(void)
{
    sntp_stop();
    if (s_conn.wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_conn.wifi_started = false;
    }
    if (s_conn.wifi_inited) {
        esp_wifi_deinit();
        s_conn.wifi_inited = false;
    }
    // 已发布的接口:持锁销毁并置 NULL,保证读者要么看到完整对象并先用完,
    // 要么看到 NULL;初始化失败的未发布对象已在回滚点直接销毁。
    netif_destroy_published();
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
    // modem sleep 默认 listen interval=3(每 3 个 beacon 醒一次),
    // BLE 共存+弱信号下 TLS 握手每轮往返都多等 ~300ms,10s 超时内
    // 完不成握手。改为每 beacon 醒一次,只增加少量功耗。
    wcfg.sta.listen_interval = 1;
    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) return err;
    return esp_wifi_connect();
}

// 事件回调:esp_event 任务上下文,只入队。
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        post_ev(Q_WIFI_STARTED, 0);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        // 捕获 reason 供配网页区分密码错误(15)/找不到 AP(200/201)等。
        const wifi_event_sta_disconnected_t *d = data;
        post_ev(Q_LOST, d ? d->reason : 0);
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP) post_ev(Q_GOT_IP, 0);
}

// 发布 FSM 状态;处于退避等待时设定有限的截止时刻。初始化失败与关联失败
// 都会走到这里,因此任务只会有界等待,不会挂死在等待启动事件上。
// WAIT_IP 同样有硬超时:默认 STA_CONNECTED 处理器(注册收包回调 + 启动
// DHCP)失效时既不会有 GOT_IP 也不会有 DISCONNECTED,必须在超时后重建
// 网络栈重试,而不是无限等待 IP。
// 此外,清理未验证期间(见 s_default_handlers_sweep_pending)进入退避时
// 必须把"整栈重建"写进重试计划:复用网络栈不会再产生 STA_START,等到
// 退避到期只重新关联会让状态停在无期限的 CONNECTING,残点永远不再清理。
static void conn_schedule(void)
{
    s_conn.state_pub = s_conn.fsm.state;
    if (s_conn.fsm.state == SUITE_CONN_WAIT_IP) {
        if (s_conn.ip_deadline == 0) {
            s_conn.ip_deadline = xTaskGetTickCount() +
                                 pdMS_TO_TICKS(CONN_IP_WAIT_TIMEOUT_MS);
        }
        return;
    }
    s_conn.ip_deadline = 0;
    if (s_conn.fsm.state != SUITE_CONN_RETRY_WAIT) return;
    if (s_default_handlers_sweep_pending) {
        // 功能证据(GOT_IP)之前,栈复用不可信(attach 前的清理没有重跑,
        // 默认 STA 动作可能仍然失效);退避到期改为整栈重建,重新执行
        // 有界残点清理并重启 STA。退避期间迟到的 GOT_IP 会先恢复在线并
        // 清除本标志(见 Q_GOT_IP),因此不会打断已经可用的栈。
        s_conn.rebuild_on_retry = true;
    }
    uint32_t wait_ms = suite_conn_backoff_ms(s_conn.fsm.retry_count);
    s_conn.retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
    ESP_LOGW(TAG, "连接失败(reason=%u),退避 %lu ms 后重试(第 %lu 次)",
             (unsigned)s_conn.fsm.last_reason, (unsigned long)wait_ms,
             (unsigned long)s_conn.fsm.retry_count);
}

static void conn_task(void *arg)
{
    (void)arg;
    queue_ev_t ev;

    suite_conn_init(&s_conn.fsm);
    suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_START);
    // esp_wifi_start() 成功后会由 STA_START 事件驱动 Q_WIFI_STARTED。
    if (wifi_stack_up() != ESP_OK) {
        // 初始化失败已完整回滚资源;进入退避等待,由重试路径重新初始化,
        // 而不是停在 CONNECTING 无限等待永远不会到来的启动事件。
        suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_INIT_FAIL);
    }
    conn_schedule();  // 发布初始状态并排出退避期限(如已失败)

    for (;;) {
        TickType_t timeout = portMAX_DELAY;
        TickType_t now = xTaskGetTickCount();
        if (s_conn.fsm.state == SUITE_CONN_RETRY_WAIT) {
            timeout = (s_conn.retry_deadline > now)
                          ? (s_conn.retry_deadline - now)
                          : 0;
        } else if (s_conn.fsm.state == SUITE_CONN_WAIT_IP) {
            timeout = (s_conn.ip_deadline > now)
                          ? (s_conn.ip_deadline - now)
                          : 0;
        }
        if (xQueueReceive(s_conn.queue, &ev, timeout) != pdTRUE) {
            if (s_conn.fsm.state == SUITE_CONN_WAIT_IP) {
                // 已关联却超过期限没有 IP:本固件只有默认 STA_CONNECTED
                // 处理器会注册收包回调并启动 DHCP,因此这里等价于默认
                // 处理器链没有生效(典型原因:销毁时清理留下禁用残点)。
                // 判定本次尝试失败,但先保留网络栈:退避期间迟到的 GOT_IP
                // 仍可恢复在线;退避到期由重试路径整栈重建,在下一次
                // attach 前重新尝试有界残点清理。绝不在这里无限等待 IP,
                // 也不把恢复押在未来某次偶然销毁上。
                ESP_LOGW(TAG, "等待 IP 超时(>%u ms),退避后重建网络栈重试",
                         (unsigned)CONN_IP_WAIT_TIMEOUT_MS);
                s_conn.rebuild_on_retry = true;
                suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_ASSOC_FAIL);
                conn_schedule();
                continue;
            }
            ev.type = Q_RETRY_NOW;  // 退避到期
            ev.reason = 0;
        }

        switch (ev.type) {
        case Q_STOP:
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_STOP);
            wifi_stack_down();
            s_conn.running = false;
            vTaskDelete(NULL);
            return;

        case Q_RETRY_NOW:
            if (s_conn.fsm.state == SUITE_CONN_RETRY_WAIT) {
                bool wifi_was_running = s_conn.wifi_started;
                if (s_conn.rebuild_on_retry) {
                    // 需要重建的两种来源:WAIT_IP 超时判定失败,或默认
                    // 处理器清理尚未获得功能证据(见 conn_schedule)。整栈
                    // 重建使 attach 前的有界清理重新尝试摘除残点,并重新
                    // 启动 STA 以产生新的 STA_START。退避期间迟到的 GOT_IP
                    // 是功能证据,会先恢复在线并清除本标志(见 Q_GOT_IP),
                    // 因此走到这里重建不会打断已经可用的栈。
                    ESP_LOGW(TAG, "重试整栈重建:重新执行默认处理器残点清理");
                    if (s_conn.wifi_started) {
                        s_conn.ignore_lost = 2;  // 吞掉本次停止的迟到断开事件
                    }
                    wifi_stack_down();
                    s_conn.rebuild_on_retry = false;
                    wifi_was_running = false;
                }
                suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_START);
                // 初始化失败后的重试(自动或手动)必须重新执行必要的
                // 网络栈初始化,不能只对尚未初始化的 Wi-Fi 调 connect。
                if (wifi_stack_up() != ESP_OK) {
                    suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_INIT_FAIL);
                } else if (wifi_was_running) {
                    // 栈本次未重启:STA_START 早已消费,直接重新关联;
                    // 栈本次重启成功则由 Q_WIFI_STARTED 触发关联。
                    if (wifi_connect() != ESP_OK) {
                        suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_ASSOC_FAIL);
                    }
                }
            }
            break;

        case Q_APPLY_NEW:
            // 手机下发了新凭据(NVS 已写好):断开/停止旧链路并重启 STA。
            // 旧链路的迟到 DISCONNECTED 在重新 STA_START 前一律吞掉。
            ESP_LOGI(TAG, "应用新 Wi-Fi 凭据,立即重新连接");
            sntp_stop();
            if (s_conn.rebuild_on_retry) {
                // 默认处理器恢复未验证:借应用新凭据的机会整栈重建,
                // 重新执行 attach 前的有界残点清理。
                if (s_conn.wifi_started) {
                    s_conn.ignore_lost = 2;
                }
                wifi_stack_down();
                s_conn.rebuild_on_retry = false;
            } else if (s_conn.wifi_started) {
                s_conn.ignore_lost = 2;  // disconnect+stop 最多两个迟到事件
                esp_wifi_disconnect();
                esp_wifi_stop();
                s_conn.wifi_started = false;
            }
            suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_APPLY_NEW);
            if (wifi_stack_up() != ESP_OK) {
                suite_conn_step(&s_conn.fsm, SUITE_CONN_EV_INIT_FAIL);
            }
            // 成功:esp_wifi_start() → STA_START(清 ignore)→ wifi_connect
            break;

        case Q_WIFI_STARTED:
            s_conn.ignore_lost = 0;  // 此后的断开都属于新一次尝试
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
            // 迟到的 GOT_IP(退避期间 DHCP 最终成功)证明网络栈与默认
            // 处理器链真实工作:取消待重建标志,不打断已经可用的栈。
            s_conn.rebuild_on_retry = false;
            if (s_default_handlers_sweep_pending) {
                // 功能证据:本固件只有默认 STA_START 处理器会启动 netif,
                // 只有默认 STA_CONNECTED 处理器会注册收包回调并启动 DHCP;
                // 没有这两步不可能收到 GOT_IP。拿到 IP 即证明默认处理器链
                // 在本代栈上真实生效,残点风险解除——依据是行为,不是注册
                // 返回值、调用次数或某次清理的 ESP_OK。
                s_default_handlers_sweep_pending = false;
                ESP_LOGI(TAG, "默认 Wi-Fi 事件处理器已由 GOT_IP 验证恢复");
            }
            portENTER_CRITICAL(&s_conn.time_mux);
            suite_time_sync_started(&s_conn.time);
            portEXIT_CRITICAL(&s_conn.time_mux);
            sntp_start();
            break;

        case Q_LOST:
            if (s_conn.ignore_lost > 0) {
                // 切换凭据过程中旧链路的残留事件:忽略,不退避。
                s_conn.ignore_lost--;
                break;
            }
            sntp_stop();
            suite_conn_step_lost(&s_conn.fsm, ev.reason);
            esp_wifi_connect();  // 立即发起;失败会再走 DISCONNECTED→退避
            break;

        default:
            break;
        }

        conn_schedule();
    }
}

// 启动失败统一回滚:回收本次启动获得的资源,使失败后可以再次
// suite_connection_start()。资源所有权:
//   - 本次获得并在回滚中回收:事件队列、netif 生命周期锁、conn 任务
//     (失败阶段不同,拥有的资源不同);
//   - 进程级共享单例:esp_netif_init() 与默认事件循环(esp_event)幂等且
//     可能已被其它模块使用,不属于本次启动,不在这里撤销;
//   - handler 实例:只有节点真正从事件循环链表摘除才算回收完成;注销
//     未完成时句柄移交 s_pending_* 槽(跨 start() 的 memset 保留),由
//     下一次 start() 在注册新处理器之前回收。
// 顺序与安全:先注销事件处理器避免迟到回调;队列与锁都先清句柄再删除
// (迟到回调看到 NULL 只会丢弃事件)。回滚只发生在任务创建成功之前,
// 此时锁没有持有者或等待者,且读者只在运行期间访问(见 ip_str),
// 因此不会出现访问已释放锁的持有者/等待者。
//
// ESP-IDF 5.5.3 的 esp_event 注销语义(已按 components/esp_event/
// esp_event.c 的 esp_event_handler_unregister_with_internal() 与
// find_and_unregister_handler() 源码核对):
//   - 事件循环锁空闲:注销在锁内立即摘除并释放节点,返回 ESP_OK;
//   - 锁忙:先把节点标记为已注销(回调从此不会再被调用),再分配一条
//     清理事件交给事件循环任务释放节点;该分配失败返回 ESP_ERR_NO_MEM,
//     此时节点已禁用但尚未释放,且不会再有清理事件投递;
//   - 对已标记的节点再次注销:锁忙路径直接返回 ESP_OK 而不补发清理
//     事件,只有锁空闲路径才真正摘除。因此 ESP_OK 不能证明节点已释放,
//     盲目重试也不能作为完成依据;
//   - 普通事件派发、重新注册同函数、注销其它处理器都不会回收该节点。
// 处理方式:注销未完成(NO_MEM 或无法确认)时保留句柄;下一次 start()
// 在注册新处理器之前用"摘除 + 再调用确认"的有限次协议完成回收,确认
// 不了就放弃本次启动——重试次数增加不会让处理器节点累积。
// 确认调用对已释放实例只做指针相等比较(handler_instances_remove /
// find_and_unregister_handler 均不解引用实例句柄),因此可以安全地
// 用它证明"节点已不在链表中"。本固件只有本模块注册
// (WIFI_EVENT, ESP_EVENT_ANY_ID)与(IP_EVENT, IP_EVENT_STA_GOT_IP),
// 不会与其他模块的实例地址复用产生误匹配。
static esp_event_handler_instance_t s_pending_wifi_handler;
static esp_event_handler_instance_t s_pending_ip_handler;

// 注销一个实例并确认节点已从事件循环链表中摘除:
//   - 返回 ESP_ERR_NOT_FOUND:节点已不在链表中,视为回收完成;
//   - 返回 ESP_OK:立即摘除与"锁忙空操作"都会返回 OK,必须再调用一次
//     确认:节点被摘除后,再调用按实例指针找不到,返回 NOT_FOUND;
//   - 返回 ESP_ERR_NO_MEM 等:节点已标记禁用但未释放,保留句柄;
//   - 两次均 OK(锁忙且清理事件刚投递的瞬态):让出一次给事件循环任务
//     释放节点后再确认一轮;仍无法确认则保留句柄。
// 返回 true 表示 *inst 已回收并置空;false 表示句柄仍需保留。
static bool conn_retire_handler(esp_event_handler_instance_t *inst,
                                esp_event_base_t base, int32_t id)
{
    for (int round = 0; round < 2; round++) {
        esp_err_t err = esp_event_handler_instance_unregister(base, id, *inst);
        if (err == ESP_ERR_NOT_FOUND) {
            *inst = NULL;
            return true;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "事件处理器注销未完成(%s),保留句柄待回收",
                     esp_err_to_name(err));
            return false;
        }
        err = esp_event_handler_instance_unregister(base, id, *inst);
        if (err == ESP_ERR_NOT_FOUND) {
            *inst = NULL;  // 上一次调用确实摘除了节点
            return true;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "事件处理器注销确认失败(%s)",
                     esp_err_to_name(err));
            return false;
        }
        // 两次都 OK 且节点仍在:可能事件循环任务即将处理已投递的清理
        // 事件。让出一次再做最后一轮确认,避免把必然释放的节点长期
        // 记在待回收槽里。
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

// 注册新处理器之前回收上一次启动遗留的延迟注销节点。单次有限尝试
// (见 conn_retire_handler 的有界确认),不忙等、不无限重试;无法确认
// 时返回 false,调用方放弃本次启动而不是叠加新的处理器节点。
static bool conn_reconcile_pending_handlers(void)
{
    if (s_pending_wifi_handler) {
        (void)conn_retire_handler(&s_pending_wifi_handler, WIFI_EVENT,
                                  ESP_EVENT_ANY_ID);
    }
    if (s_pending_ip_handler) {
        (void)conn_retire_handler(&s_pending_ip_handler, IP_EVENT,
                                  IP_EVENT_STA_GOT_IP);
    }
    return !s_pending_wifi_handler && !s_pending_ip_handler;
}

static void conn_start_rollback(void)
{
    // handler 所有权不丢失:完成注销才清空;未完成时句柄移交 pending 槽
    // (节点的回调已被 SDK 禁用)。start() 在注册前已确认 pending 槽为空,
    // 因此这里直接接管,不会覆盖尚未回收的句柄。
    if (s_conn.wifi_handler) {
        esp_event_handler_instance_t inst = s_conn.wifi_handler;
        s_conn.wifi_handler = NULL;
        if (!conn_retire_handler(&inst, WIFI_EVENT, ESP_EVENT_ANY_ID)) {
            s_pending_wifi_handler = inst;
        }
    }
    if (s_conn.ip_handler) {
        esp_event_handler_instance_t inst = s_conn.ip_handler;
        s_conn.ip_handler = NULL;
        if (!conn_retire_handler(&inst, IP_EVENT, IP_EVENT_STA_GOT_IP)) {
            s_pending_ip_handler = inst;
        }
    }
    if (s_conn.queue) {
        QueueHandle_t queue = s_conn.queue;
        s_conn.queue = NULL;
        vQueueDelete(queue);
    }
    if (s_conn.netif_lock) {
        SemaphoreHandle_t lock = s_conn.netif_lock;
        s_conn.netif_lock = NULL;
        vSemaphoreDelete(lock);
    }
    s_conn.task = NULL;
    s_conn.running = false;
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

    // netif 生命周期锁先于 conn_task 创建:任务内的发布/销毁与外部读者
    // 使用同一把锁,读者不会访问已销毁对象。
    s_conn.netif_lock = xSemaphoreCreateMutex();
    if (!s_conn.netif_lock) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    // 事件处理器注册在主任务;netif/默认循环必须先行创建(conn_task 内只做
    // 较慢的 Wi-Fi 初始化)。循环已存在时容忍 INVALID_STATE(幂等)。
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif 初始化失败: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(err));
        goto fail;
    }

    // 上一次启动失败遗留的延迟注销节点:先完成回收再注册新处理器。
    // 回收不了就拒绝本次启动,而不是叠加节点(见 conn_start_rollback)。
    if (!conn_reconcile_pending_handlers()) {
        ESP_LOGW(TAG, "事件处理器延迟注销未回收,拒绝重复注册");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    // 注册失败也要回滚:不能留下半启动状态,也不能在重试时重复注册。
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &s_conn.wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 事件处理器注册失败: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, &s_conn.ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP 事件处理器注册失败: %s", esp_err_to_name(err));
        goto fail;
    }

    if (xTaskCreate(conn_task, "suite_conn", CONN_TASK_STACK, NULL, 5,
                    &s_conn.task) != pdPASS) {
        s_conn.task = NULL;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_conn.running = true;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "连接任务启动失败,回滚本次资源: %s", esp_err_to_name(err));
    conn_start_rollback();
    return err;
}

void suite_connection_stop(void)
{
    if (s_conn.task && s_conn.running) post_ev(Q_STOP, 0);
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
    post_ev(Q_RETRY_NOW, 0);
}

esp_err_t suite_connection_apply_new_wifi(void)
{
    if (!s_conn.task || !s_conn.running) return ESP_ERR_INVALID_STATE;
    post_ev(Q_APPLY_NEW, 0);
    return ESP_OK;
}

uint16_t suite_connection_last_reason(void)
{
    return s_conn.fsm.last_reason;
}

bool suite_connection_ip_str(char *out, size_t cap)
{
    if (!out || cap < 16) return false;
    // 只在连接任务运行期间访问 netif:启动失败的回滚会释放 netif 生命周期
    // 锁(见 suite_connection_start),该门限保证读者不会与锁的创建/回收
    // 交错;模块未运行也就不存在可读的 IP。
    if (!s_conn.running || !s_conn.netif_lock) return false;
    // 持锁完成"取指针 + 查询 IP":与 netif 销毁互斥,任何任务可安全调用。
    if (xSemaphoreTake(s_conn.netif_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool ok = false;
    esp_netif_t *nif = s_conn.netif;
    if (nif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
            ok = true;
        }
    }
    (void)xSemaphoreGive(s_conn.netif_lock);
    return ok;
}

bool suite_connection_ap_rssi(int *rssi_out)
{
    if (!rssi_out || !s_conn.wifi_started) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *rssi_out = ap.rssi;
    return true;
}
