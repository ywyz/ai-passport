// tests/test_suite_weather_sem_context.c —— 天气任务桩"调用线程身份"回归。
//
// 复现旧缺陷:工作线程启动后,全局"在任务中"标记恒为 true;测试主线程
// 对不可用信号量做有界超时取用(xSemaphoreTake(empty, 20))也被当作任务
// 停靠,超过 2 秒仍不返回,并可能覆盖工作线程的停靠状态。
//
// 修复后按实际调用线程(线程本地标记)区分:非工作线程真实等待或有界
// 超时返回,工作线程的确定性调度、停靠、唤醒与退出保持不变。
// 默认与 -O2 都必须通过。
#include "weather_stubs/weather_runtime.c"

#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t s_worker_sem;
static unsigned s_worker_take_result;

static void worker_entry(void *arg)
{
    (void)arg;
    // 工作线程停在"等待 1000ms 取用"上,由测试决定以超时还是令牌唤醒。
    s_worker_take_result = (unsigned)xSemaphoreTake(s_worker_sem, 1000);
    vTaskDelete(NULL);
}

// 主线程阻塞等待的成功路径:另一个普通线程延迟投递令牌。
static SemaphoreHandle_t s_gate;

static int gate_giver(void *arg)
{
    (void)arg;
    struct timespec req = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L };
    while (thrd_sleep(&req, NULL) != 0) {
        // 被信号中断则继续睡完剩余时长(测试环境通常不会发生)。
    }
    assert(xSemaphoreGive(s_gate) == pdTRUE);
    return 0;
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (long)(b->tv_sec - a->tv_sec) * 1000L +
           (long)(b->tv_nsec - a->tv_nsec) / 1000000L;
}

int main(void)
{
    TaskHandle_t task;

    // 工作线程启动并停靠:请求 1000ms 取用。
    s_worker_sem = xSemaphoreCreateBinary();
    assert(s_worker_sem);
    assert(xTaskCreate(worker_entry, "worker", 1, NULL, 1, &task) == pdPASS);
    assert(test_task_start() == 1000);
    assert(test_task_waiting());
    assert(test_task_wait_ms() == 1000);

    // 1) 主线程对不可用信号量的有限超时:必须在超时后返回 pdFALSE,
    //    不能被误认为任务停靠而挂起(旧缺陷:超过 2s 不返回)。
    SemaphoreHandle_t empty = xSemaphoreCreateBinary();
    assert(empty);
    struct timespec t0, t1;
    assert(timespec_get(&t0, TIME_UTC) == TIME_UTC);
    assert(xSemaphoreTake(empty, 20) == pdFALSE);
    assert(timespec_get(&t1, TIME_UTC) == TIME_UTC);
    long took = elapsed_ms(&t0, &t1);
    assert(took >= 10);    // 确实等待了请求的时长,而不是立即失败
    assert(took < 1000);   // 有界返回(旧实现会一直等到测试放行)

    // 主线程的取用不得改写工作线程的停靠状态。
    assert(test_task_waiting());
    assert(test_task_wait_ms() == 1000);

    // 2) 主线程等待成功:已有令牌立即取得。
    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    assert(ready);
    assert(xSemaphoreGive(ready) == pdTRUE);
    assert(xSemaphoreTake(ready, 20) == pdTRUE);
    // 再次取用:令牌已被消费,按有界超时失败。
    assert(xSemaphoreTake(ready, 20) == pdFALSE);

    // 3) 主线程阻塞等待成功:令牌由另一个线程投递后取得。
    s_gate = xSemaphoreCreateBinary();
    assert(s_gate);
    thrd_t giver;
    assert(thrd_create(&giver, gate_giver, NULL) == thrd_success);
    assert(xSemaphoreTake(s_gate, 2000) == pdTRUE);
    assert(thrd_join(giver, NULL) == thrd_success);

    // 主线程的阻塞等待同样不得覆盖工作线程的停靠状态。
    assert(test_task_waiting());
    assert(test_task_wait_ms() == 1000);

    // 4) 工作线程仍正常停靠:投递唤醒令牌后按"等待成功"恢复并退出。
    assert(xSemaphoreGive(s_worker_sem) == pdTRUE);
    unsigned long wait = test_task_resume_wake();
    assert(wait == 0);  // 任务结束
    assert(test_task_finished());
    assert(s_worker_take_result == pdTRUE);

    puts("weather runtime semaphore thread-context tests: PASS");
    return 0;
}
