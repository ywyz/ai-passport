// tests/weather_stubs/weather_runtime.c —— 天气任务主机测试的可控运行时边界。
//
// 任务体运行在真实线程上(唯一的"任务线程"),调度由条件变量停靠/唤醒:
// 任务到达阻塞点(带超时的信号量取用或 vTaskDelay)时停靠并通知测试;
// 测试检查本次请求的等待时长,再选择"等待超时"或"已有唤醒令牌"恢复执行。
// 不使用 setjmp/longjmp 跳回已失效栈帧(那种做法在 -O2/栈保护下是未定义
// 行为)。因此调度节拍、刷新请求的合并与保留都由真实实现驱动,而不是靠
// 源码字符串断言。
//
// HTTP 客户端按脚本分块投递响应,经真实事件回调进入真实响应收集器。
#include "weather_test_stubs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define TEST_MAX_CHUNKS 8
#define TEST_MAX_SEMS 4
#define TEST_URL_MAX 192

struct test_sem {
    bool available;
    bool deleted;
};

struct test_task {
    TaskFunction_t entry;
    void *arg;
};

struct esp_http_client {
    bool live;
    esp_http_client_config_t cfg;
    int status;
};

// 脚本化响应:状态码、传输结果、分块内容与抓取过程中的钩子。
struct test_http_script {
    int status;
    esp_err_t result;
    bool init_fails;
    const char *chunks[TEST_MAX_CHUNKS];
    size_t chunk_lens[TEST_MAX_CHUNKS];
    unsigned chunk_count;
    void (*after_chunk)(void);
    unsigned performs;
};

// ---- 线程化任务与信号量状态 ----
enum task_state {
    TASK_IDLE = 0,   // 已创建、尚未启动线程
    TASK_RUNNING,    // 任务线程正在执行
    TASK_PARKED,     // 任务线程停在一次阻塞点,等待测试命令
    TASK_FINISHED,   // 任务线程已自删退出
};

static mtx_t s_lock;
static cnd_t s_cv;
static bool s_runtime_ready;
static struct test_sem s_sems[TEST_MAX_SEMS];
static unsigned s_sem_count;
static struct test_task s_task;
static thrd_t s_thread;
static enum task_state s_state = TASK_IDLE;
static bool s_task_created, s_task_started, s_task_finished, s_joined;
// 运行线程身份:线程本地标记,只有任务线程自己的调用栈可见 true,
// 测试主线程与其它线程一律为 false。必须按实际调用线程区分,不能用
// "工作线程已启动"这类全局状态——否则主线程的阻塞取用会被误判为任务
// 停靠而永久挂起(旧缺陷)。
static _Thread_local bool t_in_task;
static bool s_resume_signal;     // 本次恢复按"已有唤醒令牌"处理
static unsigned long s_wait_ms;  // 停靠点请求的等待时长(ms)
static SemaphoreHandle_t s_wait_sem;  // 停靠时等待的信号量(NULL=纯延时)
static bool s_waiting;

// ---- 脚本化 HTTP 客户端状态 ----
static struct test_http_script s_http;
static struct esp_http_client s_client;
static char s_last_url[TEST_URL_MAX];
static http_event_handle_cb s_last_handler;
static unsigned s_init_calls, s_cleanup_calls, s_init_fail_calls;

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

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 64 * 1024;
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

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

// ---- 线程化任务 ----

static int task_trampoline(void *arg)
{
    struct test_task *t = arg;
    test_lock();
    t_in_task = true;
    s_state = TASK_RUNNING;
    test_unlock();

    t->entry(t->arg);
    assert(!"weather task returned instead of deleting itself");
    return 0;
}

// 任务线程上下文:记录本次阻塞请求后停靠,直到测试下达恢复命令。
static void task_park(unsigned long wait_ms, SemaphoreHandle_t sem)
{
    assert(t_in_task);  // 只有任务线程可以进入停靠
    s_wait_ms = wait_ms;
    s_wait_sem = sem;
    s_waiting = true;
    s_state = TASK_PARKED;
    cnd_broadcast(&s_cv);
    while (s_state == TASK_PARKED) cnd_wait(&s_cv, &s_lock);
    s_waiting = false;
}

// 测试上下文:启动任务或命令其从停靠点继续,返回下一次停靠请求的等待
// 时长(ms);任务结束(自删)时返回 0。
static unsigned long task_resume(bool signal)
{
    test_lock();
    assert(s_task_created && !s_task_finished);
    if (!s_task_started) {
        s_task_started = true;
        s_state = TASK_RUNNING;
        assert(thrd_create(&s_thread, task_trampoline, &s_task) ==
               thrd_success);
    } else {
        assert(s_state == TASK_PARKED);
        s_resume_signal = signal;
        s_state = TASK_RUNNING;
        cnd_broadcast(&s_cv);
    }
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
        assert(s_waiting);
        wait = s_wait_ms;
    }
    test_unlock();
    return wait;
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority;
    test_lock();
    assert(entry && task && !s_task_created);
    s_task = (struct test_task){ .entry = entry, .arg = arg };
    s_task_created = true;
    *task = &s_task;
    test_unlock();
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL && t_in_task);  // 任务只允许自删
    test_lock();
    s_task_finished = true;
    s_state = TASK_FINISHED;
    cnd_broadcast(&s_cv);
    test_unlock();
    thrd_exit(0);
}

void vTaskDelay(TickType_t ticks)
{
    test_lock();
    assert(t_in_task);
    task_park((unsigned long)ticks, NULL);
    test_unlock();
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    test_lock();
    assert(s_sem_count < TEST_MAX_SEMS);
    SemaphoreHandle_t sem = &s_sems[s_sem_count++];
    *sem = (struct test_sem){ .available = false };
    test_unlock();
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    test_lock();
    sem->available = true;  // 互斥量创建即可取
    test_unlock();
    return sem;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    test_lock();
    assert(sem && !sem->deleted);
    // 二值信号量:已有时再次 Give 失败,重复刷新请求因此自然合并。
    BaseType_t was_available = sem->available;
    sem->available = true;
    cnd_broadcast(&s_cv);
    test_unlock();
    return was_available ? pdFALSE : pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    test_lock();
    assert(sem && !sem->deleted);
    if (sem->available) {
        sem->available = false;
        test_unlock();
        return pdTRUE;
    }
    if (ticks == 0) {
        test_unlock();
        return pdFALSE;
    }
    if (t_in_task) {
        task_park((unsigned long)ticks, sem);
        BaseType_t result = s_resume_signal ? pdTRUE : pdFALSE;
        test_unlock();
        return result;
    }
    // 非任务线程的阻塞取用(测试主线程调用 suite_weather_get 的互斥量、
    // 或回归测试主动取用空信号量):真实等待到可用或有界超时,不做成
    // 任务停靠,也不得改写任务线程的停靠状态。
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
                                      : cnd_timedwait(&s_cv, &s_lock,
                                                      &deadline);
    }
    BaseType_t result = pdFALSE;
    if (sem->available) {
        sem->available = false;
        result = pdTRUE;
    }
    test_unlock();
    return result;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    test_lock();
    assert(sem && !sem->deleted);
    sem->deleted = true;
    test_unlock();
}

// ---- HTTP 客户端桩 ----
// 这些调用全部发生在任务线程执行期间;测试仅在任务停靠时修改脚本。

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config && config->url);
    if (s_http.init_fails) {
        s_init_fail_calls++;
        return NULL;
    }
    s_init_calls++;
    assert(!s_client.live);  // 上一个客户端必须已释放
    s_client = (struct esp_http_client){ 0 };
    s_client.live = true;
    s_client.cfg = *config;
    s_client.status = s_http.status;
    snprintf(s_last_url, sizeof(s_last_url), "%s", config->url);
    s_last_handler = config->event_handler;
    return &s_client;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(client && client->live);
    s_http.performs++;
    // 失控抓取保护:一个场景内不应出现无节拍的连续抓取(如忙循环回归)。
    assert(s_http.performs <= 64);
    if (s_http.result != ESP_OK) return s_http.result;
    for (unsigned i = 0; i < s_http.chunk_count; i++) {
        esp_http_client_event_t evt = {
            .event_id = HTTP_EVENT_ON_DATA,
            .data = (void *)s_http.chunks[i],
            .data_len = (int)s_http.chunk_lens[i],
        };
        assert(s_last_handler && s_last_handler(&evt) == ESP_OK);
        if (s_http.after_chunk) s_http.after_chunk();
    }
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    assert(client && client->live);
    return client->status;
}

void esp_http_client_cleanup(esp_http_client_handle_t client)
{
    assert(client && client->live);
    client->live = false;
    s_cleanup_calls++;
}

// ---- 测试控制面 ----

void test_http_reset(void)
{
    struct test_http_script fresh = { .status = 200, .result = ESP_OK };
    s_http = fresh;
    s_last_url[0] = '\0';
    s_last_handler = NULL;
}

void test_http_set_status(int status) { s_http.status = status; }
void test_http_set_result(esp_err_t result) { s_http.result = result; }
void test_http_set_init_fails(bool fails) { s_http.init_fails = fails; }
void test_http_on_chunk(void (*hook)(void)) { s_http.after_chunk = hook; }

void test_http_add_chunk(const char *data, size_t len)
{
    assert(s_http.chunk_count < TEST_MAX_CHUNKS);
    s_http.chunks[s_http.chunk_count] = data;
    s_http.chunk_lens[s_http.chunk_count] = len;
    s_http.chunk_count++;
}

unsigned test_http_performs(void) { return s_http.performs; }
unsigned test_http_init_calls(void) { return s_init_calls; }
unsigned test_http_init_fail_calls(void) { return s_init_fail_calls; }
unsigned test_http_cleanup_calls(void) { return s_cleanup_calls; }
bool test_http_client_live(void) { return s_client.live; }
const char *test_http_last_url(void) { return s_last_url; }

// 运行任务到首次阻塞,返回随后请求的等待时长(ms)。
unsigned long test_task_start(void) { return task_resume(false); }

// 当前任务是否停在一次等待上,及其请求的等待时长(ms)。
bool test_task_waiting(void)
{
    test_lock();
    bool waiting = s_waiting;
    test_unlock();
    return waiting;
}

unsigned long test_task_wait_ms(void)
{
    test_lock();
    unsigned long wait_ms = s_wait_ms;
    test_unlock();
    return wait_ms;
}

// 等待超时后恢复:前提是等待的信号量上没有待处理令牌。
unsigned long test_task_resume_timeout(void)
{
    test_lock();
    assert(s_waiting && s_wait_sem);
    assert(!s_wait_sem->available);
    test_unlock();
    return task_resume(false);
}

// 投递唤醒后恢复:实现必须先真的给出令牌(而不只是置了标志位);
// 本次取用消费该令牌,任务按"等待成功"继续。
unsigned long test_task_resume_wake(void)
{
    test_lock();
    assert(s_waiting && s_wait_sem);
    assert(s_wait_sem->available);
    s_wait_sem->available = false;  // 唤醒被本次取用消费
    test_unlock();
    return task_resume(true);
}

bool test_task_finished(void)
{
    test_lock();
    bool finished = s_task_finished;
    test_unlock();
    return finished;
}
