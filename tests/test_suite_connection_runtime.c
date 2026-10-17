// tests/test_suite_connection_runtime.c —— Wi-Fi 连接任务运行时主机测试。
//
// 直接运行 main/suite_connection.c 的真实实现,RTOS 边界用线程化桩
// (tests/conn_stubs/conn_runtime.c):conn_task 在真实线程上停靠于队列接收,
// 测试逐次放行"退避超时"或"事件到达",并可在网络栈每一步注入失败。
// 断言对象是行为:有限退避等待、故障后完整回滚、恢复后重新初始化并联网、
// 正常连接/掉线重连/应用新凭据仍然有效,以及启动回滚中事件处理器
// "延迟注销失败"的所有权保留、有界回收与无资源增长(见文件下半部)。
// 另覆盖:WAIT_IP 期间断线插入后,未完成的默认处理器清理恢复仍保
// 有期限与后续执行机会(见文件下部的断线回归)。
#define _DEFAULT_SOURCE  // glibc strlcpy/strlcat 声明(与固件 newlib 对齐)
#include "conn_stubs/conn_runtime.c"
#include "../main/suite_connection.c"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void module_reset(void)
{
    memset(&s_conn, 0, sizeof(s_conn));
    s_conn.time_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    suite_time_tracker_init(&s_conn.time);
    // 场景间清空生产的"延迟注销待回收"槽(测试包含生产源码):
    // 每个场景都必须以自己的初始状态开始,不继承上一场景的待回收句柄。
    s_pending_wifi_handler = NULL;
    s_pending_ip_handler = NULL;
    // 默认 handlers 残点清理标志同属生产静态状态,同样按场景复位。
    s_default_handlers_sweep_pending = false;
}

static void begin_scenario(void)
{
    test_conn_reset();
    module_reset();
    assert(suite_connection_start() == ESP_OK);
}

static void begin_scenario_failing(int stage)
{
    test_conn_reset();
    test_conn_fail_at(stage);
    module_reset();
    assert(suite_connection_start() == ESP_OK);
}

static void end_scenario(void)
{
    suite_connection_stop();
    test_conn_resume_event();
    assert(test_conn_wait_park() == 0);
    assert(test_conn_finished());
    const conn_observed_t *o = test_conn_observed();
    assert(!o->netif_live && !o->driver_live && !o->defaults_live);
    assert(!o->wifi_live && !o->wifi_running);
}

// WAIT_IP 的等待上限:等待 IP 不再是无界等待——默认处理器残点导致
// DHCP 不启动时,必须在有限期限内重建网络栈重试(见生产 conn_schedule)。
#define IP_WAIT_MS ((unsigned long)CONN_IP_WAIT_TIMEOUT_MS)

// 初始连接:STA_START → 关联 → GOT_IP → ONLINE。
static void go_online(void)
{
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);  // 在线
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
}

// 起步七个阶段逐个注入失败:进入有限等待的 RETRY_WAIT、资源完整回滚;
// 修复后自动重试从零初始化并成功联网。
static void test_startup_failure_recovery(void)
{
    for (int stage = CONN_FAIL_NETIF_NEW; stage <= CONN_FAIL_WIFI_START;
         stage++) {
        test_conn_set_creds("HomeNet", "secret123");
        begin_scenario_failing(stage);

        assert(test_conn_wait_park() == 2000);  // backoff(1):有界等待
        assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
        const conn_observed_t *o = test_conn_observed();
        assert(!o->netif_live && !o->driver_live && !o->defaults_live);
        assert(!o->wifi_live && !o->wifi_running);
        assert(o->netif_destroy == o->netif_new);
        assert(o->wifi_deinit == o->wifi_init - o->wifi_init_fail);
        assert(o->wifi_stop == o->wifi_start - o->wifi_start_fail);
        assert(o->handler_regs == 2);  // 应用事件处理器不重复注册
        assert(test_conn_fail_stage() == CONN_FAIL_NONE);  // 注入已消费

        // 资源恢复:退避到期自动重试,重新初始化并联网。
        test_conn_fail_at(CONN_FAIL_NONE);
        test_conn_resume_timeout();
        assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
        assert(suite_connection_state() == SUITE_CONN_CONNECTING);
        assert(o->netif_live && o->wifi_live && o->wifi_running);
        test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
        test_conn_resume_event();
        assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
        assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
        assert(o->wifi_connect == 1);
        test_conn_ip_got_ip();
        test_conn_resume_event();
        assert(test_conn_wait_park() == UINT32_MAX);  // 在线
        assert(suite_connection_state() == SUITE_CONN_ONLINE);
        assert(o->sntp_init == 1);
        end_scenario();
    }
}

// 正常连接、掉线重连(退避内立即成功与手动立即重连两种)。
static void test_normal_connect_and_reconnect(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario();
    go_online();

    const conn_observed_t *o = test_conn_observed();
    assert(o->netif_new == 1 && o->wifi_init == 1 && o->wifi_start == 1);
    assert(o->wifi_connect == 1 && o->sntp_init == 1);
    assert(strcmp(test_conn_last_ssid(), "HomeNet") == 0);

    // IP/RSSI 可读(配网页状态卡的数据源)。
    test_conn_set_ip(0x1701A8C0);
    char ip[16];
    assert(suite_connection_ip_str(ip, sizeof(ip)));
    assert(strcmp(ip, "192.168.1.23") == 0);
    int rssi = 0;
    assert(suite_connection_ap_rssi(&rssi) && rssi == -55);

    // 掉线:停校时、进入退避,并立即发起一次重连。
    wifi_event_sta_disconnected_t d = { .reason = 15 };
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);  // backoff(1)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(suite_connection_last_reason() == 15);
    assert(o->sntp_stop == 1 && o->wifi_connect == 2);

    // 手动立即重连:不等退避,重新关联;GOT_IP 直接恢复在线。
    suite_connection_reconnect_now();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->wifi_connect == 3);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(suite_connection_last_reason() == 0);
    assert(o->sntp_init == 2);

    // 掉线后在退避到期前立即重连成功:RETRY_WAIT + GOT_IP 也恢复在线。
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 3);

    end_scenario();
}

// 在线时应用新凭据:停旧链路、吞掉迟到断开、以新凭据重新联网。
static void test_apply_new_credentials(void)
{
    test_conn_set_creds("OldNet", "oldpass");
    begin_scenario();
    go_online();

    const conn_observed_t *o = test_conn_observed();
    assert(o->sntp_init == 1);

    test_conn_set_creds("NewNet", "newpass");
    assert(suite_connection_apply_new_wifi() == ESP_OK);
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->wifi_disconnect == 1 && o->wifi_stop == 1 && o->wifi_start == 2);
    assert(o->wifi_deinit == 0);  // 未整栈重启:复用 netif/Wi-Fi 实例
    assert(o->sntp_stop == 1);

    // 旧链路的两个迟到 DISCONNECTED 被吞掉,不退避、不改状态。
    wifi_event_sta_disconnected_t d = { .reason = 15 };
    for (int i = 0; i < 2; i++) {
        test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
        test_conn_resume_event();
        assert(test_conn_wait_park() == UINT32_MAX);
        assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    }

    // 新链路 STA_START → 用新凭据关联 → 在线。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(strcmp(test_conn_last_ssid(), "NewNet") == 0);
    assert(strcmp(test_conn_last_pass(), "newpass") == 0);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 2);

    end_scenario();
}

// 无凭据:不构造无效请求,进入退避;凭据写入后 apply 立即联网。
static void test_missing_credentials_then_apply(void)
{
    test_conn_clear_creds();
    begin_scenario();
    assert(test_conn_wait_park() == UINT32_MAX);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    const conn_observed_t *o = test_conn_observed();
    assert(o->wifi_connect == 0 && o->wifi_set_config == 0);

    test_conn_set_creds("HomeNet", "secret123");
    assert(suite_connection_apply_new_wifi() == ESP_OK);
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->wifi_connect == 1);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 1);

    end_scenario();
}

// 连续初始化失败:退避递增仍为有界等待;修复后手动重连重新执行完整初始化。
static void test_init_failure_backoff_and_manual_retry(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario_failing(CONN_FAIL_WIFI_INIT);
    assert(test_conn_wait_park() == 2000);  // backoff(1)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);

    test_conn_fail_at(CONN_FAIL_WIFI_INIT);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == 4000);  // backoff(2):退避递增
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);

    // 修复资源后手动立即重连(不等退避):从 netif 开始完整重新初始化。
    test_conn_fail_at(CONN_FAIL_NONE);
    suite_connection_reconnect_now();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    const conn_observed_t *o = test_conn_observed();
    assert(o->netif_new == 3);      // 每次尝试都重新执行网络栈初始化
    assert(o->netif_destroy == 2);  // 前两次失败各自完整回收
    assert(o->wifi_deinit == 0);    // 当前一次已成功初始化
    assert(o->wifi_live && o->wifi_running);

    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 1);

    end_scenario();
}

// 停止时完整收栈:校时停止、Wi-Fi 反初始化、netif 销毁。
static void test_stop_releases_stack(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario();
    go_online();
    const conn_observed_t *o = test_conn_observed();
    assert(o->sntp_init == 1);

    suite_connection_stop();
    test_conn_resume_event();
    assert(test_conn_wait_park() == 0);
    assert(test_conn_finished());
    assert(o->sntp_stop == 1);
    assert(o->wifi_disconnect == 1 && o->wifi_stop == 1 && o->wifi_deinit == 1);
    assert(o->netif_destroy == 1);
    assert(!o->netif_live && !o->wifi_live && !o->wifi_running);
}

// ---- 回归:netif 读者与销毁的交错顺序 ----

static atomic_bool s_reader_inside;
static atomic_bool s_reader_release;
static bool s_reader_result;
static char s_reader_ip[16];

// 读侧钩子:读者已取得 netif 指针并进入 IP 查询,在此等待测试放行。
static void reader_gate_hook(void)
{
    atomic_store(&s_reader_inside, true);
    while (!atomic_load(&s_reader_release)) thrd_yield();
}

static int ip_reader_thread(void *unused)
{
    (void)unused;
    s_reader_result = suite_connection_ip_str(s_reader_ip, sizeof(s_reader_ip));
    return 0;
}

static bool wait_reader_inside(void)
{
    struct timespec start, now;
    assert(timespec_get(&start, TIME_UTC) == TIME_UTC);
    while (!atomic_load(&s_reader_inside)) {
        thrd_yield();
        assert(timespec_get(&now, TIME_UTC) == TIME_UTC);
        long ms = (long)(now.tv_sec - start.tv_sec) * 1000L +
                  (long)(now.tv_nsec - start.tv_nsec) / 1000000L;
        if (ms > 5000) return false;
    }
    return true;
}

// 读者已取得接口、销毁发生、读者继续执行:修复前销毁会先释放 netif,
// 读者恢复后继续访问已销毁对象;修复后销毁必须等待读者完成读取。
static void test_netif_reader_destroy_interleaving(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario();
    go_online();
    test_conn_set_ip(0x1701A8C0);  // 192.168.1.23

    const conn_observed_t *o = test_conn_observed();
    assert(o->netif_live);

    atomic_store(&s_reader_inside, false);
    atomic_store(&s_reader_release, false);
    s_reader_result = false;
    s_reader_ip[0] = '\0';
    test_conn_set_get_ip_info_hook(reader_gate_hook);

    thrd_t reader;
    assert(thrd_create(&reader, ip_reader_thread, NULL) == thrd_success);
    assert(wait_reader_inside());

    // 停止连接任务:销毁流程先完成驱动停止,再在读者持有的锁上等待。
    suite_connection_stop();
    test_conn_resume_event();
    assert(test_conn_wait_deinit(5000));
    // 读者仍在读取中:接口必须仍然存活,销毁尚未发生。
    assert(o->netif_live);
    assert(o->netif_destroy == 0);

    // 放行读者:读取完成后销毁才能继续;读者读到的是销毁前的有效地址。
    atomic_store(&s_reader_release, true);
    assert(thrd_join(reader, NULL) == thrd_success);
    assert(s_reader_result);
    assert(strcmp(s_reader_ip, "192.168.1.23") == 0);

    assert(test_conn_wait_park() == 0);
    assert(test_conn_finished());
    assert(o->netif_destroy == 1 && !o->netif_live);
    assert(!o->wifi_live && !o->wifi_running);

    // 销毁后的读取不访问已释放对象:安全返回 false。
    test_conn_set_get_ip_info_hook(NULL);
    char ip[16];
    assert(!suite_connection_ip_str(ip, sizeof(ip)));
}

// ---- 回归:suite_connection_start() 各阶段失败的回滚与重试 ----

// 启动阶段失败(队列/锁/netif 初始化/事件循环/处理器注册/任务创建):
// suite_connection_start() 自身返回错误,并统一回滚本次资源。
static const int k_start_stages[] = {
    CONN_FAIL_QUEUE_CREATE, CONN_FAIL_LOCK_CREATE,  CONN_FAIL_NETIF_INIT,
    CONN_FAIL_EVENT_LOOP,   CONN_FAIL_HANDLER_WIFI, CONN_FAIL_HANDLER_IP,
    CONN_FAIL_TASK_CREATE,
};

static const char *start_stage_name(int stage)
{
    switch (stage) {
    case CONN_FAIL_QUEUE_CREATE: return "queue_create";
    case CONN_FAIL_LOCK_CREATE: return "lock_create";
    case CONN_FAIL_NETIF_INIT: return "netif_init";
    case CONN_FAIL_EVENT_LOOP: return "event_loop";
    case CONN_FAIL_HANDLER_WIFI: return "handler_wifi";
    case CONN_FAIL_HANDLER_IP: return "handler_ip";
    case CONN_FAIL_TASK_CREATE: return "task_create";
    default: return "?";
    }
}

// 每个启动阶段注入一次失败:断言无本次资源遗留、无重复注册、无悬垂锁,
// 且事件循环(共享单例)不被回收;随后重试必须成功并正常联网。
static void test_start_stage_failure_rollback(void)
{
    for (size_t k = 0; k < sizeof(k_start_stages) / sizeof(k_start_stages[0]);
         k++) {
        const int stage = k_start_stages[k];
        test_conn_set_creds("HomeNet", "secret123");
        test_conn_reset();
        module_reset();

        // 失败前先建立一次真实事件循环语义:第一次 start 之外再触发
        // 一次失败,验证重试时"事件循环已存在"被容忍。
        test_conn_fail_at(stage);
        esp_err_t err = suite_connection_start();
        assert(err != ESP_OK);
        assert(test_conn_fail_stage() == CONN_FAIL_NONE);  // 注入已消费

        const conn_observed_t *o = test_conn_observed();
        // 本次资源全部回收:队列/锁/处理器/任务都不遗留。
        assert(test_conn_live_queues() == 0);
        assert(test_conn_live_mutexes() == 0);
        assert(test_conn_live_handlers() == 0);
        assert(o->queue_create == o->queue_delete);
        assert(o->mutex_create == o->mutex_delete);
        assert(o->handler_regs == o->handler_unregs);
        assert(o->handler_duplicate == 0);
        assert(o->handler_unreg_missing == 0);
        assert(!o->netif_live && !o->wifi_live && !o->wifi_running);
        // 未运行的模块不暴露 netif 读取(锁回收不会与读者交错)。
        char ip[16];
        assert(!suite_connection_ip_str(ip, sizeof(ip)));

        // 事件循环一旦创建就不回收(进程级共享单例);重试按已存在容忍。
        bool loop_created_before =
            stage != CONN_FAIL_QUEUE_CREATE &&
            stage != CONN_FAIL_LOCK_CREATE && stage != CONN_FAIL_NETIF_INIT &&
            stage != CONN_FAIL_EVENT_LOOP;
        if (loop_created_before) {
            assert(test_conn_event_loop_live());
            assert(o->event_loop_created == 1);
        }

        // 重试:无注入时必须成功;不重复注册处理器。
        unsigned regs_before = o->handler_regs;
        unsigned unregs_before = o->handler_unregs;
        assert(suite_connection_start() == ESP_OK);
        o = test_conn_observed();
        assert(test_conn_live_queues() == 1);
        assert(test_conn_live_mutexes() == 1);
        assert(test_conn_live_handlers() == 2);
        assert(o->handler_regs == regs_before + 2);
        assert(o->handler_unregs == unregs_before);
        assert(o->handler_duplicate == 0);
        assert(o->handler_unreg_missing == 0);
        if (loop_created_before) {
            assert(o->event_loop_existing == 1);
            assert(test_conn_event_loop_live());
        } else {
            assert(o->event_loop_created == 1);
        }

        go_online();
        end_scenario();
        printf("start-stage %s: rolled back and recovered\n",
               start_stage_name(stage));
    }
}

// 连续失败再恢复:资源计数不得持续增长(每次失败都完整回收,重试复用
// 槽位),最后一次成功启动的资源数与首次一致。
static void test_start_failure_no_resource_growth(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();

    const unsigned attempts = 4;
    for (unsigned i = 1; i <= attempts; i++) {
        test_conn_fail_at(CONN_FAIL_TASK_CREATE);
        assert(suite_connection_start() != ESP_OK);
        const conn_observed_t *o = test_conn_observed();
        assert(o->queue_create == i && o->queue_delete == i);
        assert(o->mutex_create == i && o->mutex_delete == i);
        assert(o->handler_regs == 2 * i && o->handler_unregs == 2 * i);
        assert(o->handler_duplicate == 0 && o->handler_unreg_missing == 0);
        assert(test_conn_live_queues() == 0 && test_conn_live_mutexes() == 0);
        assert(test_conn_live_handlers() == 0);
    }

    // 第 attempts+1 次成功:只比失败次数多一组资源,且事件循环只创建一次。
    assert(suite_connection_start() == ESP_OK);
    const conn_observed_t *o = test_conn_observed();
    assert(o->queue_create == attempts + 1 && o->queue_delete == attempts);
    assert(o->mutex_create == attempts + 1 && o->mutex_delete == attempts);
    assert(o->handler_regs == 2 * attempts + 2);
    assert(o->handler_unregs == 2 * attempts);
    assert(o->event_loop_created == 1 && o->event_loop_existing == attempts);
    assert(test_conn_live_queues() == 1 && test_conn_live_mutexes() == 1);
    assert(test_conn_live_handlers() == 2);

    go_online();
    end_scenario();
}

// ---- 回归:启动回滚中的事件处理器延迟注销失败 ----
//
// ESP-IDF 5.5.3(已按 components/esp_event/esp_event.c 源码核对):事件
// 循环锁忙时,注销先把节点标记为已注销(回调禁用),再分配一条清理事件
// 交给事件循环任务释放节点;该分配失败返回 ESP_ERR_NO_MEM,节点保留且
// 不会再有清理事件。对已标记节点再次注销,锁忙路径返回 ESP_OK 却不摘除
// ——"注销成功"不能证明节点已释放。生产必须保留句柄并在下一次启动
// 注册之前完成回收;否则重复失败会累积处理器节点。
// 本测试注入 Wi-Fi / IP / 两者 的延迟注销失败组合,断言:
//   - 失败后节点仍存在但回调被禁用,所有权可追踪(不丢失句柄);
//   - 反复重试不注册新处理器,节点数不增长;
//   - 故障解除后完成回收并重新注册,联网恢复;
//   - 预建的共享事件循环与外部模块处理器不受回滚影响。

static unsigned s_ext_invocations;

static void external_wifi_handler(void *arg, esp_event_base_t base, int32_t id,
                                  void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    s_ext_invocations++;
}

static void deferred_unregister_case(bool wifi_fault, bool ip_fault)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();
    s_ext_invocations = 0;

    // 预建共享默认事件循环与"其它模块"的 WIFI_EVENT 处理器。
    assert(esp_event_loop_create_default() == ESP_OK);
    esp_event_handler_instance_t ext = NULL;
    assert(esp_event_handler_instance_register(
               WIFI_EVENT, ESP_EVENT_ANY_ID, external_wifi_handler, NULL,
               &ext) == ESP_OK);
    assert(test_conn_live_handlers() == 1);

    // 启动在任务创建阶段失败:两个处理器已注册,回滚时按注入走延迟注销。
    test_conn_set_unreg_fault(WIFI_EVENT, wifi_fault);
    test_conn_set_unreg_fault(IP_EVENT, ip_fault);
    test_conn_fail_at(CONN_FAIL_TASK_CREATE);
    assert(suite_connection_start() != ESP_OK);

    const conn_observed_t *o = test_conn_observed();
    const unsigned expect_marked =
        (wifi_fault ? 1u : 0u) + (ip_fault ? 1u : 0u);
    // 完成注销的 handler 立即移除;未完成的被标记保留(回调禁用),
    // 句柄由生产内部记录:所有权可追踪,资源计数稳定。
    assert(test_conn_marked_handlers() == expect_marked);
    assert(test_conn_live_handlers() == 1 + expect_marked);
    assert(o->handler_unregs == 2 - expect_marked);
    assert(o->handler_unreg_marked == expect_marked);
    assert(o->handler_duplicate == 0);
    assert(o->handler_unreg_missing == 0);

    // 普通事件派发:被标记的处理器不得再收到回调(SDK 禁用语义),
    // 外部处理器照常工作;派发本身不会回收残留节点。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    assert(o->handler_invocations == 1);
    assert(s_ext_invocations == 1);
    assert(test_conn_marked_handlers() == expect_marked);
    assert(test_conn_live_handlers() == 1 + expect_marked);

    // 故障未解除时反复重试:拒绝注册新处理器,节点数不增长,
    // 不把未完成的回收当作成功。
    if (expect_marked > 0) {
        unsigned regs0 = o->handler_regs;
        for (int i = 0; i < 3; i++) {
            assert(suite_connection_start() == ESP_ERR_NO_MEM);
            o = test_conn_observed();
            assert(o->handler_regs == regs0);
            assert(test_conn_marked_handlers() == expect_marked);
            assert(test_conn_live_handlers() == 1 + expect_marked);
        }
    }

    // 故障解除:启动前先完成残留节点回收(摘除 + 确认协议),
    // 再注册新处理器;事件循环与外部处理器不受影响。
    test_conn_set_unreg_fault(WIFI_EVENT, false);
    test_conn_set_unreg_fault(IP_EVENT, false);
    assert(suite_connection_start() == ESP_OK);
    o = test_conn_observed();
    assert(test_conn_marked_handlers() == 0);
    assert(test_conn_live_handlers() == 3);  // 外部 + 本模块 Wi-Fi/IP
    assert(o->handler_regs == o->handler_unregs + test_conn_live_handlers());
    assert(o->handler_duplicate == 0 && o->handler_unreg_missing == 0);
    assert(test_conn_event_loop_live());

    // 共享循环与外部处理器仍可用;本模块重新注册的处理器恢复工作。
    go_online();
    assert(s_ext_invocations == 2);  // 手动派发 + go_online 的 STA_START
    end_scenario();
}

static void test_rollback_handler_deferred_unregister(void)
{
    deferred_unregister_case(false, false);  // 基线:正常注销路径
    deferred_unregister_case(true, false);   // 仅 Wi-Fi 延迟注销失败
    deferred_unregister_case(false, true);   // 仅 IP 延迟注销失败
    deferred_unregister_case(true, true);    // 两者同时失败
    puts("deferred unregister rollback: 4 handler-fault combinations passed");
}

// ---- 回归:默认 legacy handlers 的残点清理与恢复 ----
//
// 触发链(与复审复现一致,ESP-IDF 5.5.3,已按源码核对):
//   1. 默认 STA handlers 已注册,后续 Wi-Fi 初始化/启动失败 → 回滚销毁
//      netif → SDK clear_default_wifi_handlers();
//   2. 注销遇"事件循环锁忙 + 清理上下文分配失败":节点被标记禁用且保留,
//      不再有清理事件;SDK 忽略错误;
//   3. 再次 set_default_wifi_sta_handlers() 按函数指针匹配残点,返回
//      ESP_OK 却不重新启用 → 默认 STA_START/CONNECTED 动作(含 netif
//      启动与 DHCP)静默失效。
// 修复:销毁置位"待清理",下一次创建 netif 时在 attach 之前有界清理一次。
// 验收:恢复后默认回调与 netif/DHCP 动作真实发生;仅断言 ESP_OK、注册
// 次数或 defaults_live 不算通过(负向对照见下一个用例)。

// 一轮确定性故障/恢复:注入 wifi_start 失败 + 回滚清理"锁忙 + 单次
// 分配失败",自动重试必须清理残点并恢复默认动作;各计数精确断言(重复
// 轮次不得造成节点累积或重复回调)。
static void default_handler_fault_cycle(unsigned round)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();
    test_conn_fail_at(CONN_FAIL_WIFI_START);
    test_conn_set_defaults_clear_fault(true);
    assert(suite_connection_start() == ESP_OK);

    // 回滚完成:进入退避;残点只可能是被标记禁用的 STA_START。
    assert(test_conn_wait_park() == 2000);
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    const conn_observed_t *o = test_conn_observed();
    assert(!o->netif_live && !o->driver_live && !o->wifi_live);
    assert(o->defaults_clear_alloc_fail == 1);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 1);  // 其余默认处理器已摘除

    // 自动重试:新 netif 创建后、attach 前执行有界清理;残点被摘除,
    // 默认 handlers 重新注册为全新节点。
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS);  // 有界尝试
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 0);  // 无"匹配残点"的再注册
    assert(test_conn_live_handlers() == 2);  // 应用 instance handlers 不重复

    // 恢复后默认 STA_START/CONNECTED 回调与 netif/DHCP 动作实际发生。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 1);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 1);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->defaults_got_ip == 1);

    end_scenario();
    printf("default-handler fault cycle %u: recovered\n", round);
}

static void test_default_handler_residue_recovery(void)
{
    for (unsigned round = 1; round <= 3; round++) {
        default_handler_fault_cycle(round);
    }
    puts("default-handler residue: 3 fault/recovery rounds passed");
}

// 负向对照:只重复调用 set_default_wifi_sta_handlers() 返回 ESP_OK
// 不是恢复——残点仍被禁用(桩不许把默认资源无条件视为已释放),必须
// 由有界清理真正摘除。外部模块的处理器全程不受影响。
static void test_default_handler_reregistration_not_recovery(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();
    s_ext_invocations = 0;
    assert(esp_event_loop_create_default() == ESP_OK);
    esp_event_handler_instance_t ext = NULL;
    assert(esp_event_handler_instance_register(
               WIFI_EVENT, ESP_EVENT_ANY_ID, external_wifi_handler, NULL,
               &ext) == ESP_OK);

    test_conn_fail_at(CONN_FAIL_WIFI_START);
    test_conn_set_defaults_clear_fault(true);
    assert(suite_connection_start() == ESP_OK);
    assert(test_conn_wait_park() == 2000);
    const conn_observed_t *o = test_conn_observed();
    assert(test_conn_defaults_marked() == 1);

    // 复现 SDK 语义:再注册返回 ESP_OK,但匹配既有残点,不新增、不启用。
    unsigned dup0 = o->defaults_dup_regs;
    assert(esp_wifi_set_default_wifi_sta_handlers() == ESP_OK);
    assert(o->defaults_dup_regs == dup0 + 1);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 5);  // 其余 4 个为新增节点
    unsigned inv0 = o->defaults_invocations;
    unsigned start0 = o->defaults_netif_start;
    test_conn_default_dispatch(WIFI_EVENT_STA_START);
    assert(o->defaults_invocations == inv0);    // 禁用残点未收到回调
    assert(o->defaults_netif_start == start0);  // 没有 netif 动作

    // 修复路径(自动重试 + attach 前清理)真正摘除残点,默认动作恢复。
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(test_conn_defaults_marked() == 0);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(o->defaults_netif_start == start0 + 1);
    assert(s_ext_invocations == 1);  // 外部模块处理器照常收到事件
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    end_scenario();
    puts("default-handler re-registration alone is not recovery: PASS");
}

// 部分默认处理器注册失败(第 3 个注册失败):SDK fail 路径清理遇
// "锁忙 + 单次分配失败"留下残点;回滚销毁 netif 时的第二次清理与下一次
// 创建的 attach 前清理必须保证不累积、恢复后默认动作完整。
static void test_default_handler_partial_registration_failure(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();
    test_conn_fail_at(CONN_FAIL_DEFAULT_HANDLERS);
    test_conn_set_defaults_clear_fault(true);  // 打在 SDK fail 路径清理上
    assert(suite_connection_start() == ESP_OK);

    assert(test_conn_wait_park() == 2000);
    const conn_observed_t *o = test_conn_observed();
    assert(o->defaults_fail == 1);              // 部分注册失败已发生
    assert(o->defaults_clear_alloc_fail == 1);  // 第一次清理的分配失败
    // 回滚销毁 netif 执行第二次清理:残点被摘除,节点不累积。
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_defaults_live() == 0);
    assert(!o->netif_live && !o->driver_live && !o->defaults_live);

    // 恢复:重试经 attach 前清理后重新注册;默认动作实际发生。
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_defaults_live() == 5);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 等 IP(有界)
    assert(o->defaults_netif_start == 1);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 1);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    end_scenario();
    puts("default-handler partial registration failure: recovered");
}

// ---- 回归:残点清理"再次锁忙"不得被当作恢复 ----
//
// 触发链(与复审复现一致,ESP-IDF 5.5.3,已按源码核对):
//   1. 销毁默认 netif 时 SDK clear_default_wifi_handlers() 遇到"事件循环
//      锁忙 + 清理上下文分配失败":STA_START 节点被标记禁用、保留,且没有
//      清理事件;
//   2. 自动重试时 attach 前的补充清理再次遇到锁忙:对已标记节点 SDK 直接
//      返回 ESP_OK,不摘除、不分配、不补发清理事件。
// 修复前代码清零 pending 并继续注册:legacy 再注册命中旧节点返回成功却
// 不清除禁用标志,默认回调不执行,连接恢复会卡在无限等待 IP。
// 修复后:清理结果不构成证据,pending 保持;WAIT_IP 有硬超时,超时整体
// 重建网络栈并在下一次 attach 前重新清理;锁空闲后残点真正摘除,默认
// STA_START/CONNECTED 动作与 GOT_IP 全部真实发生后才进入 ONLINE。
// 本用例在同一模块生命周期内连续执行多轮故障与恢复(不重置、不清空证据),
// 轮次之间断言节点/netif/处理器/回调次数不累积、不重复。
static void test_default_handler_busy_sweep_recovery(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();
    s_ext_invocations = 0;
    assert(esp_event_loop_create_default() == ESP_OK);
    esp_event_handler_instance_t ext = NULL;
    assert(esp_event_handler_instance_register(
               WIFI_EVENT, ESP_EVENT_ANY_ID, external_wifi_handler, NULL,
               &ext) == ESP_OK);

    // 阶段 1:wifi_start 失败 → 回滚销毁 netif;销毁时的默认清理遇到
    // "锁忙 + 分配失败" → STA_START 被标记禁用且保留(无清理事件)。
    test_conn_fail_at(CONN_FAIL_WIFI_START);
    test_conn_set_defaults_clear_fault(true);
    assert(suite_connection_start() == ESP_OK);
    assert(test_conn_wait_park() == 2000);  // 退避(1):有界等待
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    const conn_observed_t *o = test_conn_observed();
    assert(!o->netif_live && !o->driver_live && !o->wifi_live);
    assert(o->defaults_clear_alloc_fail == 1);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 1);   // 其余默认处理器已摘除
    assert(s_default_handlers_sweep_pending); // 清理未验证:pending 保持

    // 阶段 2:连续两轮"清理时事件循环锁忙"。生产无法从返回值区分清理是否
    // 成功(SDK 语义),因此不得宣称恢复,也不得在 WAIT_IP 无限等待。
    test_conn_set_defaults_clear_busy(true);

    // 轮 A:重建后锁仍忙 → 残点保留,默认动作不发生;等待 IP 到期判定
    // 失败,进入退避(保留网络栈,迟到的 GOT_IP 仍可恢复在线)。
    unsigned mb0 = o->defaults_marked_busy_ok;
    test_conn_resume_timeout();  // 退避到期 → 重建尝试
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(s_default_handlers_sweep_pending);      // 仍未被验证
    assert(test_conn_defaults_marked() == 1);      // 残点仍在
    assert(o->defaults_marked_busy_ok ==
           mb0 + CONN_DEFAULT_SWEEP_ATTEMPTS);     // 已标记 + 锁忙:OK 但不摘除
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(test_conn_defaults_live() == 5);        // 其余为再注册节点
    assert(o->defaults_dup_regs == 1);             // legacy 命中残点
    assert(test_conn_live_handlers() == 3);        // 外部 + 本模块 Wi-Fi/IP

    // STA_START:残点被禁用 → 默认 netif 动作不发生;外部处理器照常。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);   // 等 IP(有界)
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 0);          // 无 netif 动作
    assert(o->defaults_invocations == 0);          // 残点未收到回调
    assert(s_ext_invocations == 1);                // 外部处理器不受影响

    // 已关联但没有 DHCP/收包链路:等待 IP 到期 → 判定失败,进入退避。
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == 4000);         // backoff(2)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(o->netif_live && o->wifi_live);         // 退避期间保留网络栈
    assert(test_conn_defaults_marked() == 1);
    assert(s_default_handlers_sweep_pending);
    assert(o->netif_new == 2 && o->netif_destroy == 1);

    // 轮 B:退避到期 → 整栈重建;清理再次锁忙,残点仍不摘除。
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);   // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 3 && o->netif_destroy == 2);
    // 收栈销毁时的默认清理与 attach 前清理都遇到锁忙:各计一次/三次。
    assert(o->defaults_marked_busy_ok ==
           mb0 + CONN_DEFAULT_SWEEP_ATTEMPTS + 1 + CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 2);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 2);
    assert(s_default_handlers_sweep_pending);

    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 0);          // 仍无默认 netif 动作
    assert(s_ext_invocations == 2);

    test_conn_resume_timeout();
    assert(test_conn_wait_park() == 8000);         // backoff(3)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(o->netif_live && o->wifi_live);
    assert(test_conn_defaults_marked() == 1);
    assert(s_default_handlers_sweep_pending);
    // 退避期间保留刚重建的栈:已释放的 netif 数恰好等于已创建的上一代
    // (new == destroy + 1),无泄漏、无双重释放。
    assert(o->netif_live && o->netif_new == 3 && o->netif_destroy == 2);
    printf("busy-sweep cycle B: still not recovered (marked=%u)\n",
           test_conn_defaults_marked());

    // 阶段 3:解除锁忙。下一次退避到期的整栈重建中,attach 前清理走锁空闲
    // 路径,残点被真正摘除,默认 handlers 重新注册为全新节点。
    test_conn_set_defaults_clear_busy(false);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);   // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 4 && o->netif_destroy == 3);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 3);
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 2);   // 残点摘除后无再匹配
    assert(test_conn_live_handlers() == 3);

    // 恢复后默认 STA_START/CONNECTED 回调与 netif/DHCP 动作真实发生,
    // 然后 GOT_IP 才进入 ONLINE(不以注册次数/返回值宣称恢复)。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 1);   // 默认 netif 启动动作
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 1);    // 默认 DHCP 启动动作
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->defaults_got_ip == 1);
    assert(o->sntp_init == 1);
    assert(!s_default_handlers_sweep_pending);  // 功能证据清除清理债务

    // 阶段 4:同一模块生命周期内的第二轮:在线掉线 → 立即重连恢复。
    // 轮次之间不重置,断言节点/处理器/回调不累积、不重复。
    wifi_event_sta_disconnected_t d = { .reason = 15 };
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);  // backoff(1)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(o->sntp_stop == 1);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 3);
    assert(o->netif_new == 4 && o->netif_destroy == 3);  // 多轮重建无泄漏

    test_conn_resume_timeout();  // 重试:栈仍在,直接重新关联
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 3);
    assert(test_conn_defaults_live() == 5);
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_live_handlers() == 3);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 2);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 2);

    end_scenario();
    puts("default-handler busy sweep is not recovery: verified recovery on release");
}

// 断线插入不得让"未完成的默认处理器恢复"失去期限与后续执行机会:
//   1. 销毁 netif 时清理遇"锁忙 + 清理事件分配失败",留下被禁用且保留的
//      STA_START 残点;下一次创建 netif 时 attach 前的清理再次遇锁忙
//      (已标记节点走"锁忙空操作":返回 OK 但不摘除),残点继续保留;
//   2. 默认 STA_CONNECTED 链失效时 DISCONNECTED 可能先于 30s WAIT_IP
//      期限到达:WAIT_IP → RETRY_WAIT 后,若退避到期只"重新关联"而不
//      重建栈,就既不会再产生 STA_START,也不会再进入有期限的等待,
//      任务停在无期限的 CONNECTING,残点永远不再清理(复审 P2)。
// 修复后:清理未获得功能证据(GOT_IP)期间,退避到期一律整栈重建
// (见 conn_schedule/Q_RETRY_NOW):在下一次 attach 前重新执行有界清理,
// 并重启 STA 重新产生 STA_START。
// 本用例在同一模块生命周期内连续执行两轮故障/恢复(轮次之间不 reset):
// 第一轮从启动建栈失败进入,第二轮从应用新凭据时的建栈失败进入。恢复
// 不以"注册返回 OK"或"注入 GOT_IP"为准,而是先验证默认 STA_START/
// CONNECTED 动作(netif 启动 + DHCP)真实发生,再进入 ONLINE;轮次之间
// 断言处理器节点、netif、驱动与动作计数不累积、不重复。
static void test_disconnect_before_ip_deadline_keeps_recovery(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    test_conn_reset();
    module_reset();

    // ---- 轮 1:启动建栈失败;回滚清理"锁忙 + 分配失败"留下残点 ----
    test_conn_fail_at(CONN_FAIL_WIFI_START);
    test_conn_set_defaults_clear_fault(true);
    assert(suite_connection_start() == ESP_OK);
    assert(test_conn_wait_park() == 2000);  // 退避(1):有界等待
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    const conn_observed_t *o = test_conn_observed();
    assert(o->defaults_clear_alloc_fail == 1);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 1);
    assert(s_default_handlers_sweep_pending);
    assert(!o->netif_live && !o->wifi_live);

    // ---- 轮 1:再次锁忙的重建尝试;残点不被摘除 ----
    test_conn_set_defaults_clear_busy(true);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 2 && o->netif_destroy == 1);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(o->defaults_marked_busy_ok == CONN_DEFAULT_SWEEP_ATTEMPTS);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 1);  // legacy 命中残点,未重新启用
    assert(s_default_handlers_sweep_pending);

    // ---- 轮 1:IP 期限内的断线插入(复审复现的关键顺序) ----
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // WAIT_IP:有界
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 0);  // 残点失效:默认 netif 动作未发生

    wifi_event_sta_disconnected_t d = { .reason = 201 };
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 4000);  // 退避(2):仍有界
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(suite_connection_last_reason() == 201);
    assert(s_conn.ip_deadline == 0);        // WAIT_IP 期限随状态转换结束
    assert(s_conn.rebuild_on_retry);        // 修复前为 false:恢复会失去后续机会
    assert(o->netif_live && o->wifi_live);  // 退避期间保留网络栈
    assert(o->netif_new == 2 && o->netif_destroy == 1);

    // ---- 轮 1:锁解除;自动重试必须整栈重建并真正清理残点 ----
    test_conn_set_defaults_clear_busy(false);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);  // 等重建后的 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 3 && o->netif_destroy == 2);  // 整栈重建
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 2);
    assert(test_conn_defaults_marked() == 0);  // 锁空闲:残点真正摘除
    assert(test_conn_defaults_live() == 5);    // 全新节点,无累积
    assert(o->defaults_dup_regs == 1);
    assert(s_default_handlers_sweep_pending);  // 功能证据前保持债务

    // 恢复的判定是行为:默认 STA_START 启动 netif、STA_CONNECTED 启动
    // DHCP 依次真实发生,然后才可能进入 ONLINE。
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 1);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 1);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->defaults_got_ip == 1);
    assert(o->sntp_init == 1);
    assert(!s_default_handlers_sweep_pending);  // GOT_IP 是功能证据

    // ---- 轮 2:同一生命周期内再次制造残点(应用新凭据时建栈失败) ----
    test_conn_fail_at(CONN_FAIL_WIFI_START);
    test_conn_set_defaults_clear_fault(true);
    assert(suite_connection_apply_new_wifi() == ESP_OK);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);  // 退避(1)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(o->defaults_clear_alloc_fail == 2);
    assert(test_conn_defaults_marked() == 1);
    assert(!o->netif_live && !o->wifi_live);

    // 再次锁忙 → 重建尝试;STA_START 后、IP 期限前断线。
    test_conn_set_defaults_clear_busy(true);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 4 && o->netif_destroy == 3);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 3);
    assert(o->defaults_marked_busy_ok == CONN_DEFAULT_SWEEP_ATTEMPTS * 2);
    assert(test_conn_defaults_marked() == 1);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 2);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    assert(o->defaults_netif_start == 1);  // 残点仍失效
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 4000);  // 退避(2)
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(s_conn.rebuild_on_retry);

    // ---- 轮 2:锁解除;第二轮的自动重试同样完成真实恢复 ----
    test_conn_set_defaults_clear_busy(false);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 5 && o->netif_destroy == 4);
    assert(o->defaults_sweep_calls == CONN_DEFAULT_SWEEP_ATTEMPTS * 4);
    assert(test_conn_defaults_marked() == 0);
    assert(test_conn_defaults_live() == 5);
    assert(o->defaults_dup_regs == 2);
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(o->defaults_netif_start == 2);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 2);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->defaults_got_ip == 2);
    assert(o->sntp_init == 2);
    assert(!s_default_handlers_sweep_pending);

    // ---- 两轮后无资源累积/重复:节点、netif、驱动、动作计数守恒 ----
    assert(test_conn_live_handlers() == 2);  // 应用实例处理器不重复注册
    assert(test_conn_defaults_live() == 5);
    assert(test_conn_defaults_marked() == 0);
    assert(o->handler_duplicate == 0 && o->handler_unreg_missing == 0);
    assert(o->netif_new == o->netif_destroy + 1);
    assert(o->wifi_init == o->wifi_deinit + 1);
    assert(o->wifi_start - o->wifi_start_fail == o->wifi_stop + 1);
    assert(o->defaults_regs == o->defaults_freed + test_conn_defaults_live());
    assert(o->defaults_netif_start == 2 && o->defaults_dhcp_start == 2);
    assert(o->defaults_got_ip == 2);

    end_scenario();
    puts("disconnect before IP deadline: bounded rebuild recovery kept: PASS");
}

// 正常路径的 WAIT_IP 超时:没有残点时等待 IP 同样有界;到期判定失败进入
// 退避(保留栈,迟到的 GOT_IP 仍可恢复在线),退避到期整栈重建并联网。
static void test_wait_ip_timeout_rebuild(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario();
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 有界等待 IP
    assert(suite_connection_state() == SUITE_CONN_WAIT_IP);
    const conn_observed_t *o = test_conn_observed();
    assert(o->defaults_netif_start == 1);
    assert(!s_default_handlers_sweep_pending);

    test_conn_resume_timeout();  // DHCP 一直没有结果
    assert(test_conn_wait_park() == 2000);  // backoff(1):按失败重试
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    assert(o->netif_live && o->wifi_live);  // 退避期间保留网络栈

    test_conn_resume_timeout();  // 退避到期:整栈重建并重新联网
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 2 && o->netif_destroy == 1);  // 重建只发生一次
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);
    assert(o->defaults_netif_start == 2);
    test_conn_wifi_event(WIFI_EVENT_STA_CONNECTED, NULL);
    assert(o->defaults_dhcp_start == 1);
    test_conn_ip_got_ip();
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    assert(o->sntp_init == 1);

    end_scenario();
    puts("wait-ip timeout: bounded wait and rebuild recovery: PASS");
}

// WAIT_IP 超时后迟到的 GOT_IP:退避期间 DHCP 最终成功时直接恢复在线,
// 不重建已可用的网络栈(恢复标志被功能证据清除)。
static void test_wait_ip_timeout_late_got_ip(void)
{
    test_conn_set_creds("HomeNet", "secret123");
    begin_scenario();
    assert(test_conn_wait_park() == UINT32_MAX);  // 等 STA_START
    test_conn_wifi_event(WIFI_EVENT_STA_START, NULL);
    test_conn_resume_event();
    assert(test_conn_wait_park() == IP_WAIT_MS);  // 有界等待 IP
    test_conn_resume_timeout();                   // 超时:退避等待重建
    assert(test_conn_wait_park() == 2000);
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);

    test_conn_ip_got_ip();  // 迟到但真实的 DHCP 成功
    test_conn_resume_event();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_ONLINE);
    const conn_observed_t *o = test_conn_observed();
    assert(o->netif_new == 1 && o->netif_destroy == 0);  // 未重建
    assert(o->sntp_init == 1);

    // 功能证据同时清除了重建计划:随后的掉线重连不再整栈重建。
    wifi_event_sta_disconnected_t d = { .reason = 15 };
    test_conn_wifi_event(WIFI_EVENT_STA_DISCONNECTED, &d);
    test_conn_resume_event();
    assert(test_conn_wait_park() == 2000);
    assert(suite_connection_state() == SUITE_CONN_RETRY_WAIT);
    test_conn_resume_timeout();
    assert(test_conn_wait_park() == UINT32_MAX);
    assert(suite_connection_state() == SUITE_CONN_CONNECTING);
    assert(o->netif_new == 1 && o->netif_destroy == 0);  // 仍复用原栈
    assert(o->defaults_netif_start == 1);

    end_scenario();
    puts("wait-ip timeout with late GOT_IP: no needless rebuild: PASS");
}

int main(void)
{
    test_startup_failure_recovery();
    test_start_stage_failure_rollback();
    test_start_failure_no_resource_growth();
    test_rollback_handler_deferred_unregister();
    test_default_handler_residue_recovery();
    test_default_handler_reregistration_not_recovery();
    test_default_handler_partial_registration_failure();
    test_default_handler_busy_sweep_recovery();
    test_disconnect_before_ip_deadline_keeps_recovery();
    test_wait_ip_timeout_rebuild();
    test_wait_ip_timeout_late_got_ip();
    test_normal_connect_and_reconnect();
    test_apply_new_credentials();
    test_missing_credentials_then_apply();
    test_init_failure_backoff_and_manual_retry();
    test_stop_releases_stack();
    test_netif_reader_destroy_interleaving();
    puts("connection init-failure recovery and reconnect tests: PASS");
    return 0;
}
