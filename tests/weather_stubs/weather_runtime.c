// tests/weather_stubs/weather_runtime.c —— 天气任务主机测试的可控运行时边界。
//
// 任务体在测试自己的栈上以"可恢复"方式运行:唯一阻塞点(带超时的信号量取用,
// 即 suite_weather.c 中任务循环末尾的等待)会切回测试,由测试检查本次请求的
// 等待时长,再选择"等待超时"或"已有唤醒令牌"后恢复执行。因此调度节拍、
// 刷新请求的合并与保留都由真实实现驱动,而不是靠源码字符串断言。
//
// HTTP 客户端按脚本分块投递响应,经真实事件回调进入真实响应收集器。
#include "weather_test_stubs.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

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

static struct test_sem s_sems[TEST_MAX_SEMS];
static unsigned s_sem_count;
static struct test_task s_task;
static bool s_task_created, s_task_started, s_task_finished, s_in_task;
static jmp_buf s_test_ctx, s_task_ctx;
static bool s_resume_signal;
static unsigned long s_wait_ms;
static SemaphoreHandle_t s_wait_sem;
static bool s_waiting;

// ---- 脚本化 HTTP 客户端状态 ----
static struct test_http_script s_http;
static struct esp_http_client s_client;
static char s_last_url[TEST_URL_MAX];
static http_event_handle_cb s_last_handler;
static unsigned s_init_calls, s_cleanup_calls, s_init_fail_calls;

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

// ---- 可恢复任务 ----

// 从任务上下文调用:记录阻塞请求的时长后切回测试。
static void task_park(unsigned long wait_ms, SemaphoreHandle_t sem)
{
    assert(s_in_task);
    s_wait_ms = wait_ms;
    s_wait_sem = sem;
    s_waiting = true;
    s_in_task = false;
    if (setjmp(s_task_ctx) == 0) longjmp(s_test_ctx, 1);
    s_in_task = true;
    s_waiting = false;
}

// 从测试上下文调用:运行任务到下一次阻塞并返回该次请求的等待时长(ms)。
// 任务结束(自删)时返回 0。
static unsigned long task_resume(bool signal)
{
    assert(s_task_created && !s_task_finished);
    if (!s_task_started) {
        s_task_started = true;
        s_in_task = true;
        if (setjmp(s_test_ctx) == 0) {
            s_task.entry(s_task.arg);
            assert(!"weather task returned instead of deleting itself");
        }
    } else {
        s_in_task = true;
        s_resume_signal = signal;
        if (setjmp(s_test_ctx) == 0) longjmp(s_task_ctx, 1);
    }
    s_in_task = false;
    return s_waiting ? s_wait_ms : 0;
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority;
    assert(entry && task && !s_task_created);
    s_task = (struct test_task){ .entry = entry, .arg = arg };
    s_task_created = true;
    *task = &s_task;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL && s_in_task);  // 任务只允许自删
    s_task_finished = true;
    longjmp(s_test_ctx, 1);
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_in_task);
    task_park((unsigned long)ticks, NULL);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    assert(s_sem_count < TEST_MAX_SEMS);
    SemaphoreHandle_t sem = &s_sems[s_sem_count++];
    *sem = (struct test_sem){ .available = false };
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    sem->available = true;  // 互斥量创建即可取
    return sem;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem && !sem->deleted);
    // 二值信号量:已有时再次 Give 失败,重复刷新请求因此自然合并。
    BaseType_t was_available = sem->available;
    sem->available = true;
    return was_available ? pdFALSE : pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem && !sem->deleted);
    if (sem->available) {
        sem->available = false;
        return pdTRUE;
    }
    if (ticks == 0) return pdFALSE;
    assert(s_in_task);  // 只有任务体可以阻塞等待
    task_park((unsigned long)ticks, sem);
    return s_resume_signal ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    assert(sem && !sem->deleted);
    sem->deleted = true;
}

// ---- HTTP 客户端桩 ----

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
bool test_task_waiting(void) { return s_waiting; }
unsigned long test_task_wait_ms(void) { return s_wait_ms; }

// 等待超时后恢复:前提是等待的信号量上没有待处理令牌。
unsigned long test_task_resume_timeout(void)
{
    assert(s_waiting && s_wait_sem);
    assert(!s_wait_sem->available);
    return task_resume(false);
}

// 投递唤醒后恢复:实现必须先真的给出令牌(而不只是置了标志位)。
unsigned long test_task_resume_wake(void)
{
    assert(s_waiting && s_wait_sem);
    assert(s_wait_sem->available);
    s_wait_sem->available = false;  // 唤醒被本次取用消费
    return task_resume(true);
}

bool test_task_finished(void) { return s_task_finished; }
