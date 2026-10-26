#define _DEFAULT_SOURCE
/*
 * Real translation worker and core path with external FreeRTOS/I/O stubs.
 *
 * The worker runs in a real host thread through the xTaskCreate stub. A
 * replaceable provider boundary supplies deterministic success, partial
 * failure, blocking and oversize results. The test observes only public
 * snapshots, except for the core table which is inspected to prove that the
 * real admission/result path was used.
 */
#include "../main/suite_translation.c"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct test_sem {
    pthread_mutex_t mutex;
};

static int64_t s_time_us = 1000000;
static unsigned s_random = 1;
enum {
    TRIGGER_NONE = 0,
    TRIGGER_CANCEL,
    TRIGGER_EXIT,
    TRIGGER_EXIT_REENTER,
};
static int s_trigger_action;
static int s_trigger_countdown;
static int s_arm_action;
static int s_arm_countdown;
static int s_release_action;
static unsigned s_tasks_finished;
static pthread_mutex_t s_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_cv = PTHREAD_COND_INITIALIZER;
static bool s_hold_tasks;
static bool s_release_tasks;
static bool s_fail_create;
static bool s_fail_mutex_create;
static bool s_fail_core_mutex_once;
static unsigned s_mutex_calls;
static unsigned s_tasks_created;

typedef struct {
    TaskFunction_t entry;
    void *arg;
} task_start_t;

static void *task_trampoline(void *arg)
{
    task_start_t *start = arg;
    pthread_mutex_lock(&s_gate);
    while (s_hold_tasks && !s_release_tasks)
        pthread_cond_wait(&s_gate_cv, &s_gate);
    pthread_mutex_unlock(&s_gate);
    start->entry(start->arg);
    free(start);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name;
    (void)stack;
    (void)priority;
    pthread_mutex_lock(&s_gate);
    bool fail = s_fail_create;
    if (!fail) s_tasks_created++;
    pthread_mutex_unlock(&s_gate);
    if (fail) return pdFAIL;
    task_start_t *start = malloc(sizeof(*start));
    assert(start);
    start->entry = entry;
    start->arg = arg;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, task_trampoline, start) == 0);
    assert(pthread_detach(thread) == 0);
    if (task) *task = (TaskHandle_t)1;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    /* The test worker returns from its entry function instead of pthread_exit
       so the trampoline can release its start record. */
    assert(task == NULL);
    s_tasks_finished++;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {
        .tv_sec = (time_t)(ticks / 1000U),
        .tv_nsec = (long)(ticks % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    unsigned call = s_mutex_calls++;
    if (s_fail_mutex_create || (s_fail_core_mutex_once && call == 1U))
        return NULL;
    struct test_sem *sem = calloc(1, sizeof(*sem));
    assert(sem);
    assert(pthread_mutex_init(&sem->mutex, NULL) == 0);
    return sem;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem);
    if (ticks == 0)
        return pthread_mutex_trylock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem);
    if (sem == s_mutex && s_release_action != TRIGGER_NONE &&
        !s_view.worker_busy && s_view.source[0]) {
        int action = s_release_action;
        s_release_action = TRIGGER_NONE;
        if (action == TRIGGER_CANCEL)
            suite_translation_cancel();
        else if (action == TRIGGER_EXIT)
            suite_translation_session_exit();
        else if (action == TRIGGER_EXIT_REENTER) {
            suite_translation_session_exit();
            suite_translation_session_enter();
        }
    }
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    if (!sem) return;
    assert(pthread_mutex_destroy(&sem->mutex) == 0);
    free(sem);
}

int64_t esp_timer_get_time(void)
{
    if (s_trigger_countdown > 0 && --s_trigger_countdown == 0) {
        int action = s_trigger_action;
        s_trigger_action = TRIGGER_NONE;
        if (action == TRIGGER_CANCEL)
            suite_translation_cancel();
        else if (action == TRIGGER_EXIT)
            suite_translation_session_exit();
        else if (action == TRIGGER_EXIT_REENTER) {
            suite_translation_session_exit();
            suite_translation_session_enter();
        }
    }
    return s_time_us;
}

void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)s_random++;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "stub error";
}

void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static pthread_mutex_t s_provider_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_provider_cv = PTHREAD_COND_INITIALIZER;
static enum {
    PROVIDER_OK = 0,
    PROVIDER_TRANSLATE_FAILED,
    PROVIDER_RECOGNIZE_FAILED,
    PROVIDER_BLOCK,
    PROVIDER_OVERSIZE,
    PROVIDER_EXACT_BOUNDARY,
    PROVIDER_UNTERMINATED
} s_provider_mode;
static bool s_provider_entered;
static bool s_provider_release;
static unsigned s_provider_calls;

static bool copy_text(char *out, size_t cap, const char *text)
{
    size_t length = strlen(text);
    if (length + 1U > cap) return false;
    memcpy(out, text, length + 1U);
    return true;
}

static void arm_publication_trigger(void)
{
    s_trigger_action = s_arm_action;
    s_trigger_countdown = s_arm_countdown;
    s_arm_action = TRIGGER_NONE;
    s_arm_countdown = 0;
}

static suite_translation_provider_result_t test_provider_run(
    tr_direction_t direction, char *source, size_t source_cap, char *target,
    size_t target_cap)
{
    pthread_mutex_lock(&s_provider_lock);
    s_provider_calls++;
    s_provider_entered = true;
    pthread_cond_broadcast(&s_provider_cv);
    while (s_provider_mode == PROVIDER_BLOCK && !s_provider_release)
        pthread_cond_wait(&s_provider_cv, &s_provider_lock);
    int mode = (int)s_provider_mode;
    pthread_mutex_unlock(&s_provider_lock);

    if (mode == PROVIDER_RECOGNIZE_FAILED) {
        source[0] = 0;
        target[0] = 0;
        return SUITE_TR_PROVIDER_RECOGNIZE_FAILED;
    }
    const char *en = "Good morning. The train leaves at nine.";
    const char *zh = "早上好。火车九点出发。";
    if (mode == PROVIDER_TRANSLATE_FAILED) {
        assert(copy_text(source, source_cap,
                         direction == TR_DIRECTION_ZH_EN ? zh : en));
        target[0] = 0;
        arm_publication_trigger();
        return SUITE_TR_PROVIDER_TRANSLATE_FAILED;
    }
    if (mode == PROVIDER_OVERSIZE) {
        memset(source, 'a', 2048);
        memset(target, 'b', 2048);
        source[2048] = 0;
        target[2048] = 0;
        return SUITE_TR_PROVIDER_OK;
    }
    if (mode == PROVIDER_EXACT_BOUNDARY || mode == PROVIDER_UNTERMINATED) {
        for (size_t i = 0; i < 2048; i += 4) {
            memcpy(source + i, "\xF0\x9F\x98\x80", 4);
            memcpy(target + i, "\xF0\x9F\x98\x80", 4);
        }
        source[2048] = mode == PROVIDER_EXACT_BOUNDARY ? 0 : 'x';
        target[2048] = mode == PROVIDER_EXACT_BOUNDARY ? 0 : 'x';
        return SUITE_TR_PROVIDER_OK;
    }
    assert(copy_text(source, source_cap,
                     direction == TR_DIRECTION_ZH_EN ? zh : en));
    assert(copy_text(target, target_cap,
                     direction == TR_DIRECTION_ZH_EN ? en : zh));
    arm_publication_trigger();
    return SUITE_TR_PROVIDER_OK;
}

static const suite_translation_provider_t s_test_provider = {
    .name = "host-test-provider",
    .run = test_provider_run,
};

static void provider_reset(unsigned mode)
{
    pthread_mutex_lock(&s_provider_lock);
    s_provider_mode = mode;
    s_provider_entered = false;
    s_provider_release = false;
    s_provider_calls = 0;
    pthread_mutex_unlock(&s_provider_lock);
}

static void provider_release(void)
{
    pthread_mutex_lock(&s_provider_lock);
    s_provider_release = true;
    pthread_cond_broadcast(&s_provider_cv);
    pthread_mutex_unlock(&s_provider_lock);
}

static void gate_reset(void)
{
    pthread_mutex_lock(&s_gate);
    s_hold_tasks = false;
    s_release_tasks = false;
    s_fail_create = false;
    s_fail_mutex_create = false;
    s_fail_core_mutex_once = false;
    s_mutex_calls = 0;
    s_tasks_created = 0;
    pthread_mutex_unlock(&s_gate);
}

static void gate_release_tasks(void)
{
    pthread_mutex_lock(&s_gate);
    s_release_tasks = true;
    pthread_cond_broadcast(&s_gate_cv);
    pthread_mutex_unlock(&s_gate);
}

static bool snapshot(suite_translation_view_t *out)
{
    const suite_translation_view_t *view = suite_translation_view_acquire();
    if (!view) return false;
    *out = *view;
    suite_translation_view_release();
    return true;
}

static bool wait_entered(void)
{
    for (unsigned i = 0; i < 3000; ++i) {
        pthread_mutex_lock(&s_provider_lock);
        bool entered = s_provider_entered;
        pthread_mutex_unlock(&s_provider_lock);
        if (entered) return true;
        usleep(1000);
    }
    return false;
}

static bool wait_until(suite_translation_state_t state, bool busy,
                       suite_translation_view_t *out)
{
    for (unsigned i = 0; i < 3000; ++i) {
        if (snapshot(out) && out->state == state && out->worker_busy == busy)
            return true;
        usleep(1000);
    }
    return false;
}

static bool wait_worker_idle(suite_translation_view_t *out)
{
    for (unsigned i = 0; i < 3000; ++i) {
        if (snapshot(out) && !out->worker_busy) return true;
        usleep(1000);
    }
    return false;
}

static void clear_module_storage(void)
{
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_core_mutex) {
        vSemaphoreDelete(s_core_mutex);
        s_core_mutex = NULL;
    }
    s_started = false;
    s_mutex_calls = 0;
    atomic_store(&s_session, 1U);
    atomic_store(&s_identity_epoch, 1U);
    atomic_store(&s_cancel_requested, false);
    atomic_store(&s_exit_invalidated, false);
    atomic_store(&s_enter_pending, false);
    atomic_store(&s_dispatch_failed, false);
    atomic_store(&s_reconcile_required, false);
    atomic_store(&s_enter_session, 0U);
    memset(&s_view, 0, sizeof(s_view));
    memset(&s_core, 0, sizeof(s_core));
    s_provider = &suite_translation_mock_provider;
    s_pending_direction = TR_DIRECTION_EN_ZH;
    s_pending_session = 0;
    s_pending_epoch = 0;
}

static void reset_module(void)
{
    suite_translation_view_t current;
    assert(wait_worker_idle(&current));
    clear_module_storage();
    s_time_us = 1000000;
    s_random = 1;
    s_trigger_action = TRIGGER_NONE;
    s_trigger_countdown = 0;
    s_arm_action = TRIGGER_NONE;
    s_arm_countdown = 0;
    s_release_action = TRIGGER_NONE;
    s_tasks_finished = 0;
    gate_reset();
    provider_reset(PROVIDER_OK);
    assert(suite_translation_start() == ESP_OK);
    suite_translation_session_enter();
    assert(suite_translation_set_provider(&s_test_provider));
}

static void start_job(tr_direction_t direction)
{
    assert(suite_translation_select(direction));
    assert(suite_translation_run());
}

static void test_direction_success_and_separation(void)
{
    reset_module();
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.direction == TR_DIRECTION_EN_ZH);
    assert(strstr(view.source, "Good morning"));
    assert(strstr(view.target, "早上好"));
    assert(strcmp(view.source, view.target));
    assert(!strcmp(view.status, "本机翻译完成"));
    assert(tr_core_count(&s_core) == 1);
    assert(s_core.jobs[0].state == TR_STATE_DONE);
    assert(s_core.jobs[0].has_source == 1 && s_core.jobs[0].has_target == 1);

    suite_translation_ready();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    assert(!view.source[0] && !view.target[0]);
    start_job(TR_DIRECTION_ZH_EN);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.direction == TR_DIRECTION_ZH_EN);
    assert(strstr(view.source, "早上好"));
    assert(strstr(view.target, "Good morning"));
    assert(tr_core_count(&s_core) == 2);
    assert(s_core.jobs[0].state == TR_STATE_DONE);
    assert(s_core.jobs[1].state == TR_STATE_DONE);
}

static void test_failure_retains_recognized_source(void)
{
    reset_module();
    provider_reset(PROVIDER_TRANSLATE_FAILED);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    assert(strstr(view.source, "Good morning"));
    assert(!view.target[0]);
    assert(strstr(view.status, "源文"));
    assert(view.expires_at_ms > 0);
    assert(s_core.jobs[0].state == TR_STATE_FAILED);
    assert(s_core.jobs[0].has_source == 1);
    assert(s_core.jobs[0].has_target == 0);
}

static void test_partial_failure_exact_ttl_boundary(void)
{
    reset_module();
    provider_reset(PROVIDER_TRANSLATE_FAILED);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    int64_t expiry = view.expires_at_ms;
    assert(expiry > 0 && view.source[0] && !view.target[0]);

    s_time_us = expiry * 1000 - 1;
    suite_translation_poll();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_FAILED);
    assert(view.source[0] && !view.target[0]);

    s_time_us = expiry * 1000;
    suite_translation_poll();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_EXPIRED);
    assert(!view.source[0] && !view.target[0]);
    assert(view.expires_at_ms == 0);
}

static void test_exact_2048_byte_512_scalar_output_accepted(void)
{
    reset_module();
    provider_reset(PROVIDER_EXACT_BOUNDARY);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(strlen(view.source) == TR_TEXT_MAX_BYTES);
    assert(strlen(view.target) == TR_TEXT_MAX_BYTES);
    assert(s_core.jobs[0].state == TR_STATE_DONE);
    assert(s_core.jobs[0].source_bytes == TR_TEXT_MAX_BYTES);
    assert(s_core.jobs[0].target_bytes == TR_TEXT_MAX_BYTES);
}

static void test_unterminated_provider_output_rejected(void)
{
    reset_module();
    provider_reset(PROVIDER_UNTERMINATED);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(strstr(view.status, "校验"));
    assert(s_core.jobs[0].state == TR_STATE_FAILED);
    assert(s_core.jobs[0].has_source == 0);
    assert(s_core.jobs[0].has_target == 0);
}

static void test_recognize_failure_clears_text(void)
{
    reset_module();
    provider_reset(PROVIDER_RECOGNIZE_FAILED);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(s_core.jobs[0].state == TR_STATE_FAILED);
    assert(s_core.jobs[0].has_source == 0);
    assert(s_core.jobs[0].has_target == 0);
}

static void test_oversize_result_fails_closed(void)
{
    reset_module();
    provider_reset(PROVIDER_OVERSIZE);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(strstr(view.status, "校验"));
}

static void test_cancel_before_dispatch(void)
{
    reset_module();
    pthread_mutex_lock(&s_gate);
    s_hold_tasks = true;
    pthread_mutex_unlock(&s_gate);
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_cancel();
    suite_translation_view_t view;
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_CANCELLED);
    gate_release_tasks();
    assert(wait_until(SUITE_TRANSLATION_CANCELLED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(s_provider_calls == 0);
    assert(tr_core_count(&s_core) == 0);
    assert(s_tasks_created == 1);
}

static void test_cancel_during_work_and_new_job(void)
{
    reset_module();
    provider_reset(PROVIDER_BLOCK);
    start_job(TR_DIRECTION_ZH_EN);
    assert(wait_entered());
    suite_translation_view_t view;
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_WORKING);
    suite_translation_cancel();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_CANCELLED);
    provider_release();
    assert(wait_until(SUITE_TRANSLATION_CANCELLED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(s_core.jobs[0].state == TR_STATE_CANCELLED);
    assert(s_provider_calls == 1);

    provider_reset(PROVIDER_OK);
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(s_core.jobs[0].state == TR_STATE_CANCELLED);
    assert(s_core.jobs[1].state == TR_STATE_DONE);
}

static void test_exit_reentry_rejects_late_result(void)
{
    reset_module();
    provider_reset(PROVIDER_BLOCK);
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_entered());
    suite_translation_session_exit();
    suite_translation_session_enter();
    provider_release();
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_IDLE, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(s_core.jobs[0].state == TR_STATE_CANCELLED);
    assert(s_core.count == 1);

    provider_reset(PROVIDER_OK);
    start_job(TR_DIRECTION_ZH_EN);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.direction == TR_DIRECTION_ZH_EN);
    assert(strstr(view.source, "早上好"));
    assert(s_core.count == 2);
    assert(s_core.jobs[1].state == TR_STATE_DONE);
}

static void test_rapid_reentry_blocks_new_job_until_ack(void)
{
    reset_module();
    provider_reset(PROVIDER_BLOCK);
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_entered());
    suite_translation_session_exit();
    suite_translation_session_enter();
    assert(!suite_translation_run());
    assert(s_provider_calls == 1);
    provider_release();
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_IDLE, false, &view));
    provider_reset(PROVIDER_OK);
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(s_provider_calls == 1);
}

static void test_expiry_clears_ram_text_only(void)
{
    reset_module();
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.source[0] && view.target[0]);
    s_time_us += (int64_t)(SUITE_TRANSLATION_TTL_MS + 1) * 1000;
    suite_translation_poll();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_EXPIRED);
    assert(!view.source[0] && !view.target[0]);
    assert(strstr(view.status, "过期"));
    assert(s_core.jobs[0].state == TR_STATE_DONE);
    assert(s_core.jobs[0].has_source == 1 && s_core.jobs[0].has_target == 1);

    suite_translation_ready();
    start_job(TR_DIRECTION_ZH_EN);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(s_core.count == 2);
}

static void test_eight_admissions_remain_navigable(void)
{
    reset_module();
    for (unsigned i = 0; i < TR_METADATA_SLOTS; ++i) {
        if (i) {
            suite_translation_ready();
            assert(suite_translation_select(i % 2 ? TR_DIRECTION_ZH_EN
                                                  : TR_DIRECTION_EN_ZH));
        }
        assert(suite_translation_run());
        suite_translation_view_t view;
        assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    }
    assert(tr_core_count(&s_core) == TR_METADATA_SLOTS);

    suite_translation_ready();
    suite_translation_view_t view;
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    assert(suite_translation_select(TR_DIRECTION_ZH_EN));
    assert(suite_translation_run());
    assert(wait_until(SUITE_TRANSLATION_BUSY, false, &view));
    assert(strstr(view.status, "8/8"));
    assert(!view.worker_busy);
    unsigned old_epoch = view.identity_epoch;
    assert(tr_core_count(&s_core) == TR_METADATA_SLOTS);

    /* Explicit mock-only recovery: a deliberate reset starts a fresh identity
       epoch and lets the ninth synthetic job run without a reboot. It only
       reinitializes this volatile in-firmware mock table. */
    assert(suite_translation_reset_mock_session());
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    assert(view.identity_epoch > old_epoch);
    assert(tr_core_count(&s_core) == 0);
    assert(suite_translation_select(TR_DIRECTION_EN_ZH));
    assert(suite_translation_run());
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(tr_core_count(&s_core) == 1);
    assert(strstr(view.source, "Good morning"));
}

static void test_mutex_allocation_failure_recovers(void)
{
    suite_translation_view_t view;
    assert(wait_worker_idle(&view));
    clear_module_storage();
    s_time_us = 1000000;
    s_random = 1;
    s_trigger_action = TRIGGER_NONE;
    s_trigger_countdown = 0;
    s_arm_action = TRIGGER_NONE;
    s_arm_countdown = 0;
    s_release_action = TRIGGER_NONE;
    s_tasks_finished = 0;
    provider_reset(PROVIDER_OK);
    s_fail_mutex_create = true;
    assert(suite_translation_start() == ESP_ERR_NO_MEM);
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_UNAVAILABLE);
    s_fail_mutex_create = false;
    assert(suite_translation_start() == ESP_OK);
    suite_translation_session_enter();
    assert(suite_translation_set_provider(&s_test_provider));
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
}

static void test_core_mutex_allocation_failure_recovers(void)
{
    suite_translation_view_t view;
    assert(wait_worker_idle(&view));
    clear_module_storage();
    s_time_us = 1000000;
    s_random = 1;
    provider_reset(PROVIDER_OK);
    s_fail_core_mutex_once = true;
    assert(suite_translation_start() == ESP_ERR_NO_MEM);
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_UNAVAILABLE);
    s_fail_core_mutex_once = false;
    s_mutex_calls = 0;
    assert(suite_translation_start() == ESP_OK);
    suite_translation_session_enter();
    assert(suite_translation_set_provider(&s_test_provider));
    start_job(TR_DIRECTION_EN_ZH);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
}

static void test_worker_allocation_failure_recovers(void)
{
    reset_module();
    suite_translation_test_fail_alloc(true);
    assert(suite_translation_run());
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_FAILED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(strstr(view.status, "内存"));
    assert(!view.worker_busy);
    suite_translation_test_fail_alloc(false);
    start_job(TR_DIRECTION_ZH_EN);
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.direction == TR_DIRECTION_ZH_EN);
}

static void test_exit_wipes_full_text_buffers(void)
{
    reset_module();
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.source[0] && view.target[0]);
    suite_translation_session_exit();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    for (size_t i = 0; i < sizeof(view.source); ++i)
        assert(view.source[i] == 0);
    for (size_t i = 0; i < sizeof(view.target); ++i)
        assert(view.target[i] == 0);
    assert(view.expires_at_ms == 0);
}

static void publication_race_case(unsigned provider_mode, int action,
                                 int countdown, bool expect_idle)
{
    reset_module();
    provider_reset(provider_mode);
    s_arm_action = action;
    s_arm_countdown = countdown;
    start_job(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    if (expect_idle)
        assert(wait_until(SUITE_TRANSLATION_IDLE, false, &view));
    else
        assert(wait_until(SUITE_TRANSLATION_CANCELLED, false, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(!view.worker_busy);
    assert(atomic_load(&s_cancel_requested) || action != TRIGGER_CANCEL);
}

static void test_cancel_at_core_and_view_publication_boundaries(void)
{
    publication_race_case(PROVIDER_OK, TRIGGER_CANCEL, 1, false);
    publication_race_case(PROVIDER_OK, TRIGGER_CANCEL, 2, false);
    publication_race_case(PROVIDER_TRANSLATE_FAILED, TRIGGER_CANCEL, 2, false);
}

static void test_exit_and_reentry_at_view_publication_boundary(void)
{
    publication_race_case(PROVIDER_OK, TRIGGER_EXIT, 2, true);
    publication_race_case(PROVIDER_TRANSLATE_FAILED, TRIGGER_EXIT, 1, true);
    publication_race_case(PROVIDER_OK, TRIGGER_EXIT_REENTER, 2, true);

    /* The race above must not poison the next job. */
    suite_translation_view_t view;
    provider_reset(PROVIDER_OK);
    assert(suite_translation_select(TR_DIRECTION_ZH_EN));
    assert(suite_translation_run());
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(strstr(view.source, "早上好"));
}

static void wait_task_finished(unsigned expected)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (s_tasks_finished >= expected) return;
        usleep(1000);
    }
    assert(0 && "worker did not quiesce");
}

static void assert_text_full_wipe(void)
{
    for (size_t i = 0; i < sizeof(s_view.source); ++i)
        assert(s_view.source[i] == 0);
    for (size_t i = 0; i < sizeof(s_view.target); ++i)
        assert(s_view.target[i] == 0);
}

static void cleanup_release_case(unsigned provider_mode, int action)
{
    reset_module();
    provider_reset(provider_mode);
    s_release_action = action;
    unsigned before = s_tasks_finished;
    start_job(TR_DIRECTION_EN_ZH);
    wait_task_finished(before + 1);
    /* Intentionally inspect the module directly: no snapshot or poll runs
       after worker quiescence, matching the offscreen-retention repro. */
    assert(!s_view.worker_busy);
    assert_text_full_wipe();
    assert(atomic_load(&s_exit_invalidated) == false);
    assert(atomic_load(&s_enter_pending) == false);
}

static void test_cleanup_release_cancel_and_exit_no_snapshot(void)
{
    cleanup_release_case(PROVIDER_OK, TRIGGER_CANCEL);
    cleanup_release_case(PROVIDER_TRANSLATE_FAILED, TRIGGER_CANCEL);
    cleanup_release_case(PROVIDER_OK, TRIGGER_EXIT);
    cleanup_release_case(PROVIDER_TRANSLATE_FAILED, TRIGGER_EXIT);
    cleanup_release_case(PROVIDER_OK, TRIGGER_EXIT_REENTER);

    /* Immediate re-entry from the cleaned surface can run a new job. */
    suite_translation_view_t view;
    assert(suite_translation_set_provider(&s_test_provider));
    provider_reset(PROVIDER_OK);
    suite_translation_session_enter();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    assert(suite_translation_select(TR_DIRECTION_ZH_EN));
    assert(suite_translation_run());
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(strstr(view.source, "早上好"));
}

static void test_task_create_failure_recovers(void)
{
    reset_module();
    pthread_mutex_lock(&s_gate);
    s_fail_create = true;
    pthread_mutex_unlock(&s_gate);
    assert(!suite_translation_run());
    suite_translation_view_t view;
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_UNAVAILABLE);
    assert(!view.worker_busy);
    assert(strstr(view.status, "内存"));

    pthread_mutex_lock(&s_gate);
    s_fail_create = false;
    pthread_mutex_unlock(&s_gate);
    assert(suite_translation_select(TR_DIRECTION_ZH_EN));
    assert(suite_translation_run());
    assert(wait_until(SUITE_TRANSLATION_DONE, false, &view));
    assert(view.direction == TR_DIRECTION_ZH_EN);
}

int main(void)
{
    test_direction_success_and_separation();
    test_failure_retains_recognized_source();
    test_partial_failure_exact_ttl_boundary();
    test_exact_2048_byte_512_scalar_output_accepted();
    test_unterminated_provider_output_rejected();
    test_recognize_failure_clears_text();
    test_oversize_result_fails_closed();
    test_cancel_before_dispatch();
    test_cancel_during_work_and_new_job();
    test_exit_reentry_rejects_late_result();
    test_rapid_reentry_blocks_new_job_until_ack();
    test_expiry_clears_ram_text_only();
    test_eight_admissions_remain_navigable();
    test_mutex_allocation_failure_recovers();
    test_core_mutex_allocation_failure_recovers();
    test_worker_allocation_failure_recovers();
    test_exit_wipes_full_text_buffers();
    test_cancel_at_core_and_view_publication_boundaries();
    test_exit_and_reentry_at_view_publication_boundary();
    test_cleanup_release_cancel_and_exit_no_snapshot();
    test_task_create_failure_recovers();
    puts("translation worker/core cancellation, direction, failure, expiry, "
         "bounds and reentry: PASS");
    return 0;
}
