#define _DEFAULT_SOURCE
#include "suite_translation.h"
#include "translation_real_stubs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct test_sem { pthread_mutex_t mutex; };
static int64_t s_time_us = 1000000;
static unsigned s_random = 1;
static unsigned s_tasks_finished;
static bool s_fail_create;
static bool s_fast_deadline;

static void *task_trampoline(void *arg)
{
    TaskFunction_t entry = (TaskFunction_t)arg;
    entry(NULL);
    return NULL;
}
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    if (s_fail_create) return pdFAIL;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, task_trampoline, (void *)entry) == 0);
    assert(pthread_detach(thread) == 0);
    if (task) *task = (TaskHandle_t)1;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); s_tasks_finished++; }
void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = { .tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000L };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    s_time_us += (int64_t)ticks * 1000;
    if (s_fast_deadline) s_time_us += 10000000;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct test_sem *sem = calloc(1, sizeof(*sem));
    assert(sem); assert(pthread_mutex_init(&sem->mutex, NULL) == 0);
    return sem;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (ticks == 0) return pthread_mutex_trylock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    if (!sem) return;
    assert(pthread_mutex_destroy(&sem->mutex) == 0);
    free(sem);
}
int64_t esp_timer_get_time(void) { return s_time_us; }
void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)s_random++;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "stub"; }
void test_log(const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
}

static bool snapshot(suite_translation_view_t *out)
{
    const suite_translation_view_t *view = suite_translation_view_acquire();
    if (!view) return false;
    *out = *view;
    suite_translation_view_release();
    return true;
}
static bool wait_state(suite_translation_state_t state, suite_translation_view_t *out)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (snapshot(out) && out->state == state && !out->worker_busy) return true;
        usleep(1000);
    }
    return false;
}
static bool wait_not_busy(suite_translation_view_t *out)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (snapshot(out) && !out->worker_busy) return true;
        usleep(1000);
    }
    return false;
}
static bool wait_recording_second(suite_translation_view_t *out)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (snapshot(out) && out->state == SUITE_TRANSLATION_RECORDING &&
            out->capture_seconds >= 1)
            return true;
        usleep(1000);
    }
    return false;
}
static void reset_view(void)
{
    suite_translation_view_t view;
    for (unsigned i = 0; i < 5000; ++i) {
        if (snapshot(&view) && !view.worker_busy) break;
        usleep(1000);
    }
    suite_translation_ready();
    assert(suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL));
}
static void start(tr_direction_t direction)
{
    assert(suite_translation_select(direction));
    assert(suite_translation_run());
}
static void set_status_done(bool source, bool target)
{
    tr_stub.status_result = SUITE_TR_HTTP_OK;
    tr_stub.status_count = 1;
    tr_stub.status_index = 0;
    tr_stub.status_states[0] = SUITE_TR_PROTOCOL_STATE_DONE;
    tr_stub.status_source = source;
    tr_stub.status_target = target;
}
static void set_status_failed(bool source)
{
    tr_stub.status_result = SUITE_TR_HTTP_OK;
    tr_stub.status_count = 1;
    tr_stub.status_index = 0;
    tr_stub.status_states[0] = SUITE_TR_PROTOCOL_STATE_FAILED;
    tr_stub.status_source = source;
    tr_stub.status_target = false;
}

static void assert_alloc_failure(unsigned mask)
{
    translation_real_stub_reset();
    reset_view();
    suite_translation_test_fail_alloc_mask(mask);
    tr_stub.heap_free_bytes = 50000U;
    tr_stub.heap_largest_bytes = 4096U;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    char expected[96];
    snprintf(expected, sizeof(expected),
             "无法准备翻译缓存，内存不足\nA%u F50000 L4096", mask);
    assert(!strcmp(view.status, expected));
    assert(strlen(view.status) < sizeof(view.status));
    assert(memchr(view.status, 0, sizeof(view.status)) != NULL);
    assert(!view.source[0] && !view.target[0]);
    assert(!view.tls_diag.valid);
    assert(view.expires_at_ms == 0);
    assert(!view.worker_busy);
    /* The deferred text allocation runs after a successful ready call. */
    assert(tr_stub.http_ready_calls == 1U && tr_stub.acquire_calls == 0U);
    assert(tr_stub.stage_calls == 0U && tr_stub.audio_release_calls == 0U);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 0U);
    /* Preflight diag + first-failure sample + failed diag. */
    assert(tr_stub.heap_free_calls == 3U && tr_stub.heap_largest_calls == 3U);
    assert(tr_stub.heap_free_first == 50000U);
    assert(tr_stub.heap_largest_first == 4096U);
    suite_translation_test_fail_alloc_mask(0U);
}

static void test_allocation_failure_numeric_evidence(void)
{
    const unsigned masks[] = { 2U, 4U, 6U };
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i)
        assert_alloc_failure(masks[i]);

    /* Keep the legacy boolean hook's all-fail semantics for text buffers. */
    translation_real_stub_reset();
    reset_view();
    suite_translation_test_fail_alloc(true);
    tr_stub.heap_free_bytes = 50000U;
    tr_stub.heap_largest_bytes = 4096U;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "A6 F50000 L4096"));
    suite_translation_test_fail_alloc(false);
}

static void test_allocation_failure_recovery_and_exit(void)
{
    const unsigned partial_masks[] = { 2U, 4U };
    for (unsigned cycle = 0; cycle < sizeof(partial_masks) / sizeof(partial_masks[0]);
         ++cycle) {
        assert_alloc_failure(partial_masks[cycle]);
        translation_real_stub_reset();
        reset_view();
        set_status_done(true, true);
        start(TR_DIRECTION_EN_ZH);
        suite_translation_view_t view;
        assert(wait_recording_second(&view));
        suite_translation_finish_recording();
        assert(wait_state(SUITE_TRANSLATION_DONE, &view));
        assert(!strcmp(view.source, "Hello"));
        assert(!strcmp(view.target, "你好"));
        assert(!strcmp(view.status, "翻译完成"));
        assert(!strstr(view.status, "\nA"));
    }

    suite_translation_session_exit();
    suite_translation_view_t view;
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
    assert(!view.source[0] && !view.target[0]);
    assert(!strstr(view.status, "\nA"));
    suite_translation_session_enter();
    assert(suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL));

    /* A later cancel path still works after an allocation failure. */
    assert_alloc_failure(2U);
    translation_real_stub_reset();
    reset_view();
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_cancel();
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(!strstr(view.status, "\nA"));
}

static void test_allocation_failure_cancel_precedence(void)
{
    translation_real_stub_reset();
    reset_view();
    suite_translation_test_fail_alloc_mask(2U);
    tr_stub.heap_free_bytes = 50000U;
    tr_stub.heap_largest_bytes = 4096U;
    tr_stub.cancel_on_heap_sample = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.heap_hook_fired);
    assert(!strstr(view.status, "\nA"));
    assert(!view.source[0] && !view.target[0]);
    suite_translation_test_fail_alloc_mask(0U);

    /* Session exit also wins over the allocation failure. */
    translation_real_stub_reset();
    reset_view();
    suite_translation_test_fail_alloc_mask(2U);
    tr_stub.heap_free_bytes = 50000U;
    tr_stub.heap_largest_bytes = 4096U;
    tr_stub.exit_on_heap_sample = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_not_busy(&view));
    assert(tr_stub.heap_hook_fired);
    assert(view.state == SUITE_TRANSLATION_IDLE ||
           view.state == SUITE_TRANSLATION_CANCELLED);
    assert(!strstr(view.status, "\nA"));
    assert(!view.source[0] && !view.target[0]);
    suite_translation_test_fail_alloc_mask(0U);
}

static void assert_ready_failure(suite_translation_http_result_t result,
                                 unsigned http_status, bool ready_payload,
                                 unsigned expected_r, unsigned expected_h,
                                 unsigned expected_b)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_result = result;
    tr_stub.ready_http_status = http_status;
    tr_stub.ready_payload = ready_payload;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    char expected[96];
    snprintf(expected, sizeof(expected), "翻译服务不可用\nR%u H%u B%u",
             expected_r, expected_h, expected_b);
    assert(!strcmp(view.status, expected));
    assert(strlen(view.status) < sizeof(view.status));
    assert(memchr(view.status, 0, sizeof(view.status)) != NULL);
    assert(!view.source[0] && !view.target[0]);
    assert(view.expires_at_ms == 0);
    assert(!view.worker_busy);
    assert(tr_stub.http_ready_calls == 1U);
    assert(tr_stub.acquire_calls == 0U && tr_stub.stage_calls == 0U);
    assert(tr_stub.audio_release_calls == 0U);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 0U);
}


static void assert_tls_ready_failure(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_result = SUITE_TR_HTTP_E_TLS;
    tr_stub.ready_http_status = 0;
    tr_stub.ready_payload = false;
    tr_stub.ready_tls_diag_valid = true;
    tr_stub.ready_tls_diag = (suite_translation_tls_diag_t){
        .valid = true,
        .stage = SUITE_TR_TLS_STAGE_TCP_SO_ERROR,
        .sys_source = SUITE_TR_TLS_SYS_SO_ERROR,
        .local_errno = 0,
        .so_error = 111,
        .sdk_sys_errno = 0,
        .sys_error = 111,
        .esp_err_code = 0x3000,
        .tls_code = -0x7F00,
        .verify_flags = 0x8000,
        .free_bytes = 50000,
        .largest_bytes = 4000,
    };
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(!strcmp(view.status, "翻译服务不可用\nR7 S8 X0"));
    assert(view.tls_diag.valid);
    assert(view.tls_diag.stage == SUITE_TR_TLS_STAGE_TCP_SO_ERROR);
    assert(view.tls_diag.sys_source == SUITE_TR_TLS_SYS_SO_ERROR);
    assert(view.tls_diag.so_error == 111);
    assert(view.tls_diag.sys_error == 111);
    assert(view.tls_diag.esp_err_code == 0x3000);
    assert(view.tls_diag.tls_code == -0x7F00);
    assert(view.tls_diag.verify_flags == 0x8000);
    assert(view.tls_diag.free_bytes == 50000);
    assert(view.tls_diag.largest_bytes == 4000);
    assert(!view.source[0] && !view.target[0]);
    assert(tr_stub.http_ready_calls == 1U && tr_stub.acquire_calls == 0U);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 0U);
}

static void test_readiness_failure_numeric_evidence(void)
{
    assert_tls_ready_failure();
    assert_ready_failure(SUITE_TR_HTTP_OK, 200U, false, 0U, 200U, 0U);
    assert_ready_failure(SUITE_TR_HTTP_E_NOT_READY, 503U, false, 30U, 503U, 0U);

    /* A following normal run clears the readiness diagnostic. */
    translation_real_stub_reset();
    reset_view();
    set_status_done(true, true);
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_DONE, &view));
    assert(!strcmp(view.status, "翻译完成"));
    assert(!view.tls_diag.valid);
    assert(!strstr(view.status, "\nR") && !strstr(view.status, "\nA"));

    /* Cancel/exit at the one readiness call still wins over the failure. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_tls_diag_valid = true;
    tr_stub.ready_tls_diag.valid = true;
    tr_stub.ready_tls_diag.stage = SUITE_TR_TLS_STAGE_TCP_SELECT;
    tr_stub.cancel_during_ready = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(!view.tls_diag.valid);
    assert(!strstr(view.status, "\nR"));
    assert(tr_stub.http_admit_calls == 0U && tr_stub.acquire_calls == 0U);

    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_tls_diag_valid = true;
    tr_stub.ready_tls_diag.valid = true;
    tr_stub.ready_tls_diag.stage = SUITE_TR_TLS_STAGE_TCP_SELECT;
    tr_stub.exit_during_ready = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_not_busy(&view));
    assert(view.state == SUITE_TRANSLATION_IDLE ||
           view.state == SUITE_TRANSLATION_CANCELLED);
    assert(!view.tls_diag.valid);
    assert(!strstr(view.status, "\nR"));
    assert(!view.source[0] && !view.target[0]);
}

static void test_success_order_and_identity(void)
{
    translation_real_stub_reset();
    reset_view();
    set_status_done(true, true);
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_DONE, &view));
    assert(view.mode == SUITE_TRANSLATION_MODE_REAL);
    assert(!strcmp(view.source, "Hello"));
    assert(!strcmp(view.target, "你好"));
    assert(view.job_id[0] && view.operation_id[0]);
    assert(view.server_generation == 3);
    assert(view.expires_at_ms > 0);
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
    assert(tr_stub.http_status_calls == 1 && tr_stub.http_cancel_calls == 0);
    assert(tr_stub.upload_reader_bytes > 0);
    assert(tr_stub.cleanup_calls == 1);
    char expected_sha[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(expected_sha + i * 2, 3, "%02x", i + 1);
    expected_sha[64] = 0;
    assert(!strcmp(tr_stub.admitted_sha, expected_sha));
    /* ready -> acquire -> stage -> audio-init -> audio-release -> admit ->
       upload -> scratch-cleanup -> poll */
    bool saw_ready = false, saw_release = false, saw_admit = false;
    bool saw_upload = false, saw_cleanup = false, saw_status = false;
    for (unsigned i = 0; i < tr_stub.order_count; ++i) {
        int value = tr_stub.order[i];
        if (value == TR_ORDER_READY) saw_ready = true;
        if (value == TR_ORDER_AUDIO_RELEASE) saw_release = true;
        if (value == TR_ORDER_ADMIT) { saw_admit = true; assert(saw_ready && saw_release); }
        if (value == TR_ORDER_UPLOAD) { saw_upload = true; assert(saw_admit); }
        if (value == TR_ORDER_CLEANUP) { saw_cleanup = true; assert(saw_upload); }
        if (value == TR_ORDER_STATUS) { saw_status = true; assert(saw_cleanup); }
    }
    assert(saw_ready && saw_release && saw_admit && saw_upload && saw_cleanup && saw_status);
}

static void test_format_failure_releases_audio(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.audio_format_fail = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "录音启动失败"));
    assert(tr_stub.audio_release_calls == 1);
    assert(tr_stub.http_ready_calls == 1 && tr_stub.http_admit_calls == 0);
    assert(tr_stub.cleanup_calls == 1);

    translation_real_stub_reset();
    reset_view();
    tr_stub.audio_init_fail = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "录音启动失败"));
    assert(tr_stub.audio_release_calls == 1);
    assert(tr_stub.http_admit_calls == 0);
    assert(tr_stub.cleanup_calls == 1);
}

static void test_cancel_during_ready_admit_and_status(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.cancel_during_ready = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.acquire_calls == 0 && tr_stub.http_admit_calls == 0);
    assert(tr_stub.cleanup_calls == 0 && tr_stub.http_cancel_calls == 0);

    translation_real_stub_reset();
    reset_view();
    tr_stub.cancel_during_admit = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 0);
    assert(tr_stub.cleanup_calls == 1 && tr_stub.http_cancel_calls == 0);

    translation_real_stub_reset();
    reset_view();
    tr_stub.cancel_during_status = true;
    set_status_done(true, true);
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
    assert(tr_stub.cleanup_calls == 1 && tr_stub.http_cancel_calls == 1);
}

static void test_cancel_during_upload_deletes_known_job(void)
{
    translation_real_stub_reset();
    reset_view();
    set_status_done(true, true);
    tr_stub.cancel_during_upload = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
    assert(tr_stub.http_status_calls == 0);
    assert(tr_stub.http_cancel_calls == 1);
    assert(tr_stub.cleanup_calls == 1);
}

static void test_tiny_audio_no_post(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.pcm_max = 5000;
    tr_stub.status_count = 1;
    tr_stub.status_states[0] = SUITE_TR_PROTOCOL_STATE_DONE;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "录音不足"));
    assert(tr_stub.http_admit_calls == 0 && tr_stub.http_upload_calls == 0);
    assert(tr_stub.cleanup_calls == 1);
}

static void test_release_failure_no_tls(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.audio_release_fail = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "录音结束失败"));
    assert(tr_stub.http_admit_calls == 0);
    assert(tr_stub.cleanup_calls == 1);
}

static void test_ten_second_cap(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.pcm_max = 320000;
    tr_stub.read_delay_us = 500;
    set_status_done(true, true);
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_DONE, &view));
    assert(tr_stub.pcm_bytes == 320000);
    assert(tr_stub.audio_reads == 625);
    assert(tr_stub.upload_reader_bytes == 320044);
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_cancel_calls == 0);
}

static void test_cleanup_failure_is_visible(void)
{
    translation_real_stub_reset();
    reset_view();
    set_status_done(true, true);
    tr_stub.cleanup_fail = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "录音清理失败"));
    assert(tr_stub.cleanup_calls == 1);
    assert(tr_stub.http_status_calls == 0);
}

static void test_identity_mismatch_and_deadline_unknown(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.admit_bad_identity = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "服务结果异常"));
    assert(tr_stub.http_upload_calls == 0);

    translation_real_stub_reset();
    reset_view();
    tr_stub.status_result = SUITE_TR_HTTP_OK;
    tr_stub.status_count = 1;
    tr_stub.status_states[0] = SUITE_TR_PROTOCOL_STATE_RUNNING;
    s_fast_deadline = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    s_fast_deadline = false;
    assert(tr_stub.http_status_calls > 0);
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
}

static void test_missing_config_has_no_fallback(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.config_status = SUITE_TRANSLATION_CONFIG_UNCONFIGURED;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_UNAVAILABLE, &view));
    assert(strstr(view.status, "USB配置"));
    assert(tr_stub.http_ready_calls == 0 && tr_stub.acquire_calls == 0);
}

static void test_offline_and_untrusted_stop_before_network(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.online = false;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "离线"));
    assert(tr_stub.http_ready_calls == 0);

    translation_real_stub_reset();
    reset_view();
    tr_stub.time_trusted = false;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "校时"));
    assert(tr_stub.http_ready_calls == 0);
}

static void test_ambiguous_post_and_put(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.admit_result = SUITE_TR_HTTP_E_IO;
    tr_stub.admit_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 0);
    assert(tr_stub.cleanup_calls == 1);

    translation_real_stub_reset();
    reset_view();
    tr_stub.upload_result = SUITE_TR_HTTP_E_IO;
    tr_stub.upload_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
    assert(tr_stub.cleanup_calls == 1);
}

static bool valid_operation_id(const char *id)
{
    if (!id || strlen(id) != 32U) return false;
    for (size_t i = 0; i < 32U; ++i) {
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    }
    return true;
}

static void test_ambiguous_buffer_outcome(void)
{
    static const char ambiguous_oom[] = "内存不足，服务结果未知";
    suite_translation_view_t view;

    /* Admit: the POST may have reached the server before the allocation
       failed. Publish UNKNOWN, keep the OOM cause and operation identity. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.admit_result = SUITE_TR_HTTP_E_BUFFER;
    tr_stub.admit_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(!strcmp(view.status, ambiguous_oom));
    assert(strstr(view.status, "内存不足") != NULL);
    assert(strstr(view.status, "服务结果未知") != NULL);
    assert(strlen(view.status) < sizeof(view.status));
    assert(valid_operation_id(view.operation_id));
    assert(!view.job_id[0]);
    assert(view.server_generation == 0);
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 1U);

    /* Upload: keep the known job identity and never replay or DELETE. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.upload_result = SUITE_TR_HTTP_E_BUFFER;
    tr_stub.upload_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(!strcmp(view.status, ambiguous_oom));
    assert(strstr(view.status, "内存不足") != NULL);
    assert(strstr(view.status, "服务结果未知") != NULL);
    assert(strlen(view.status) < sizeof(view.status));
    assert(valid_operation_id(view.operation_id));
    assert(view.job_id[0]);
    assert(view.server_generation == 3U);
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 1U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 1U);

    /* Status in flight: UNKNOWN, OOM visible, known job preserved. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.status_result = SUITE_TR_HTTP_E_BUFFER;
    tr_stub.status_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(!strcmp(view.status, ambiguous_oom));
    assert(strstr(view.status, "内存不足") != NULL);
    assert(strstr(view.status, "服务结果未知") != NULL);
    assert(strlen(view.status) < sizeof(view.status));
    assert(valid_operation_id(view.operation_id));
    assert(view.job_id[0]);
    assert(view.server_generation == 3U);
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 1U);
    assert(tr_stub.http_status_calls == 1U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 1U);

    /* A non-buffer late ambiguity keeps the generic UNKNOWN text. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.status_result = SUITE_TR_HTTP_E_IO;
    tr_stub.status_ambiguous = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_UNKNOWN, &view));
    assert(!strcmp(view.status, "服务结果未知，请稍后"));
    assert(view.job_id[0]);
    assert(tr_stub.http_status_calls == 1U && tr_stub.http_cancel_calls == 0U);
}

static void test_repeat_record_cancel_then_done(void)
{
    suite_translation_view_t view;

    translation_real_stub_reset();
    reset_view();
    for (unsigned cycle = 0; cycle < 3U; ++cycle) {
        start(TR_DIRECTION_EN_ZH);
        assert(wait_recording_second(&view));
        suite_translation_cancel();
        assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
        assert(!view.worker_busy);
        assert(!view.source[0] && !view.target[0]);
        assert(view.expires_at_ms == 0);
        assert(tr_stub.acquire_calls == cycle + 1U);
        assert(tr_stub.audio_release_calls == cycle + 1U);
        assert(tr_stub.cleanup_calls == cycle + 1U);
        assert(tr_stub.http_admit_calls == 0U);
        assert(tr_stub.http_cancel_calls == 0U);

        /* Leave and return to the page between attempts. */
        suite_translation_session_exit();
        assert(snapshot(&view) && view.state == SUITE_TRANSLATION_IDLE);
        assert(!view.worker_busy);
        suite_translation_session_enter();
        assert(suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL));
    }

    /* A normal job after the repeated cancellations still completes. */
    set_status_done(true, true);
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_DONE, &view));
    assert(!strcmp(view.source, "Hello"));
    assert(!strcmp(view.target, "你好"));
    assert(!view.worker_busy);
    assert(view.expires_at_ms > 0);
    assert(tr_stub.acquire_calls == 4U);
    assert(tr_stub.audio_release_calls == 4U);
    assert(tr_stub.cleanup_calls == 4U);
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 1U);
    assert(tr_stub.http_status_calls == 1U && tr_stub.http_cancel_calls == 0U);
}

static void test_cancel_and_exit_after_hash_before_admit(void)
{
    suite_translation_view_t view;

    /* User cancellation arrives after hashing, before the admit POST. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.cancel_on_sha = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(!view.worker_busy);
    assert(!view.operation_id[0] && !view.job_id[0]);
    assert(!view.source[0] && !view.target[0]);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 1U);
    assert(tr_stub.audio_release_calls == 1U);

    /* Page exit in the same window: no identity publication and no POST. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.exit_on_sha = true;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_not_busy(&view));
    assert(view.state == SUITE_TRANSLATION_IDLE ||
           view.state == SUITE_TRANSLATION_CANCELLED);
    assert(!view.operation_id[0] && !view.job_id[0]);
    assert(!view.source[0] && !view.target[0]);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);
    assert(tr_stub.cleanup_calls == 1U);
    assert(tr_stub.audio_release_calls == 1U);

    suite_translation_session_enter();
    assert(suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL));
}

static void test_expired_terminal_state(void)
{
    translation_real_stub_reset();
    reset_view();
    tr_stub.status_result = SUITE_TR_HTTP_OK;
    tr_stub.status_count = 1;
    tr_stub.status_states[0] = SUITE_TR_PROTOCOL_STATE_EXPIRED;
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_EXPIRED, &view));
    assert(!view.source[0] && !view.target[0]);
    assert(tr_stub.http_admit_calls == 1 && tr_stub.http_upload_calls == 1);
}

static void test_partial_source_ttl(void)
{
    translation_real_stub_reset();
    reset_view();
    set_status_failed(true);
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.source, "Hello"));
    assert(!view.target[0]);
    assert(view.expires_at_ms > 0);
    int64_t expiry = view.expires_at_ms;
    s_time_us = expiry * 1000;
    suite_translation_poll();
    assert(snapshot(&view) && view.state == SUITE_TRANSLATION_EXPIRED);
    assert(!view.source[0] && !view.target[0]);
}

static void test_cancel_recording_and_config_interlock(void)
{
    translation_real_stub_reset();
    reset_view();
    start(TR_DIRECTION_EN_ZH);
    suite_translation_view_t view;
    assert(wait_recording_second(&view));
    suite_translation_cancel();
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.http_admit_calls == 0);
    assert(tr_stub.cleanup_calls == 1);

    translation_real_stub_reset();
    reset_view();
    tr_stub.usb_window_open = true;
    assert(!suite_translation_run());
    assert(tr_stub.acquire_calls == 0);
    tr_stub.usb_window_open = false;
}

static void test_transport_buffer_failure_mapping(void)
{
    suite_translation_view_t view;

    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_result = SUITE_TR_HTTP_E_BUFFER;
    tr_stub.ready_payload = false;
    tr_stub.ready_http_status = 0;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(!strcmp(view.status, "无法准备翻译缓存，内存不足"));
    assert(tr_stub.http_ready_calls == 1U && tr_stub.acquire_calls == 0U);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);

    /* A ready failure must not reach the deferred text allocation. */
    translation_real_stub_reset();
    reset_view();
    tr_stub.ready_result = SUITE_TR_HTTP_E_TLS;
    tr_stub.ready_payload = false;
    suite_translation_test_fail_alloc_mask(6U);
    start(TR_DIRECTION_EN_ZH);
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(strstr(view.status, "翻译服务不可用"));
    assert(!strstr(view.status, "\nA"));
    assert(tr_stub.http_ready_calls == 1U && tr_stub.acquire_calls == 0U);
    assert(tr_stub.http_admit_calls == 0U && tr_stub.http_upload_calls == 0U);
    suite_translation_test_fail_alloc_mask(0U);

    translation_real_stub_reset();
    reset_view();
    tr_stub.admit_result = SUITE_TR_HTTP_E_BUFFER;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(!strcmp(view.status, "无法准备翻译缓存，内存不足"));
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 0U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);

    translation_real_stub_reset();
    reset_view();
    tr_stub.upload_result = SUITE_TR_HTTP_E_BUFFER;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(!strcmp(view.status, "无法准备翻译缓存，内存不足"));
    assert(tr_stub.http_admit_calls == 1U && tr_stub.http_upload_calls == 1U);
    assert(tr_stub.http_status_calls == 0U && tr_stub.http_cancel_calls == 0U);

    translation_real_stub_reset();
    reset_view();
    tr_stub.status_result = SUITE_TR_HTTP_E_BUFFER;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_FAILED, &view));
    assert(!strcmp(view.status, "无法准备翻译缓存，内存不足"));
    assert(tr_stub.http_status_calls == 1U && tr_stub.http_cancel_calls == 0U);

    translation_real_stub_reset();
    reset_view();
    tr_stub.cancel_during_status = true;
    tr_stub.cancel_result = SUITE_TR_HTTP_E_BUFFER;
    start(TR_DIRECTION_EN_ZH);
    assert(wait_recording_second(&view));
    suite_translation_finish_recording();
    assert(wait_state(SUITE_TRANSLATION_CANCELLED, &view));
    assert(tr_stub.http_cancel_calls == 1U);
    assert(!strstr(view.status, "\nA") && !strstr(view.status, "\nR"));
}

int main(void)
{
    assert(suite_translation_start() == ESP_OK);
    suite_translation_session_enter();
    assert(suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL));
    test_success_order_and_identity();
    test_ten_second_cap();
    test_format_failure_releases_audio();
    test_tiny_audio_no_post();
    test_release_failure_no_tls();
    test_cleanup_failure_is_visible();
    test_missing_config_has_no_fallback();
    test_offline_and_untrusted_stop_before_network();
    test_ambiguous_post_and_put();
    test_ambiguous_buffer_outcome();
    test_repeat_record_cancel_then_done();
    test_cancel_and_exit_after_hash_before_admit();
    test_cancel_during_ready_admit_and_status();
    test_cancel_during_upload_deletes_known_job();
    test_identity_mismatch_and_deadline_unknown();
    test_expired_terminal_state();
    test_partial_source_ttl();
    test_cancel_recording_and_config_interlock();
    test_allocation_failure_numeric_evidence();
    test_allocation_failure_recovery_and_exit();
    test_allocation_failure_cancel_precedence();
    test_transport_buffer_failure_mapping();
    test_readiness_failure_numeric_evidence();
    puts("real translation worker capture/transport/identity/cleanup: PASS");
    return 0;
}
