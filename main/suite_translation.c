#include "suite_translation.h"

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "suite_audio_job.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_translation_config.h"
#include "suite_translation_http.h"
#include "suite_translation_usb.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TR_WORKER_STACK 8192U
#define TR_WORKER_PRIORITY 3U
#define TR_MOCK_WAV_BYTES (44U + TR_WAV_MIN_DATA_BYTES)
#define TR_MOCK_TOKEN "mock_translation_capability_0123456789abcdef"

static const char *TAG = "suite_translation";

/* Snapshot lock: short view/status copies only. */
static SemaphoreHandle_t s_mutex;
/* Core lock: all suite_translation_core calls, including WAV hashing. */
static SemaphoreHandle_t s_core_mutex;
static suite_translation_view_t s_view;
static tr_core_t s_core;
static tr_capability_t s_capability;
static const suite_translation_provider_t *s_provider =
    &suite_translation_mock_provider;
static bool s_started;
static atomic_bool s_cancel_requested;
static atomic_bool s_exit_invalidated;
static atomic_bool s_enter_pending;
static atomic_bool s_dispatch_failed;
static atomic_bool s_reconcile_required;
static atomic_uint s_enter_session;
static atomic_uint s_session = 1U;
static atomic_uint s_identity_epoch = 1U;
static tr_direction_t s_pending_direction = TR_DIRECTION_EN_ZH;
static suite_translation_mode_t s_pending_mode = SUITE_TRANSLATION_MODE_MOCK;
static unsigned s_pending_session;
static unsigned s_pending_epoch;
static atomic_bool s_capture_finish;
static int64_t s_diag_start_us;

static bool apply_enter_locked(unsigned session);
static void invalidate_core_nonblocking(void);

static const suite_translation_view_t s_unavailable = {
    .state = SUITE_TRANSLATION_UNAVAILABLE,
    .direction = TR_DIRECTION_EN_ZH,
    .mode = SUITE_TRANSLATION_MODE_MOCK,
    .mock = true,
    .worker_busy = false,
    .identity_epoch = 1U,
    .status = "翻译任务不可用",
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void wipe_memory(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static void set_status_locked(const char *text)
{
    if (!text) text = "";
    snprintf(s_view.status, sizeof(s_view.status), "%s", text);
}

static void clear_text_locked(void)
{
    wipe_memory(s_view.source, sizeof(s_view.source));
    wipe_memory(s_view.target, sizeof(s_view.target));
    memset(&s_view.tls_diag, 0, sizeof(s_view.tls_diag));
}

static void publish_locked(suite_translation_state_t state, const char *status)
{
    s_view.state = state;
    set_status_locked(status);
    s_view.serial++;
}

/*
 * TTL applies to every retained plaintext field. A partial FAILED result keeps
 * recognized source text; that text expires on the same RAM-only deadline as a
 * completed result. Metadata/dedup state is intentionally not cleared here.
 */
static void expire_locked(int64_t now)
{
    if (!s_view.expires_at_ms || now < s_view.expires_at_ms) return;
    if (!s_view.source[0] && !s_view.target[0]) {
        s_view.expires_at_ms = 0;
        return;
    }
    clear_text_locked();
    s_view.expires_at_ms = 0;
    publish_locked(SUITE_TRANSLATION_EXPIRED, "结果已过期，请重新翻译");
}

/* Reconcile an atomic cancel/exit that raced a publication while the snapshot
   lock was held by the worker. The worker also checks before acknowledging
   cleanup; this covers an intent set after that check or while it holds the
   lock, so a late result cannot remain visible or retained. */
static bool reconcile_pending_locked(void)
{
    bool invalidate = false;
    if (atomic_exchange(&s_enter_pending, false)) {
        invalidate = apply_enter_locked(atomic_load(&s_enter_session));
    }
    if (atomic_exchange(&s_dispatch_failed, false)) {
        s_view.worker_busy = false;
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_UNAVAILABLE,
                       "无法创建任务，内存不足");
    }
    bool exited = atomic_load(&s_exit_invalidated);
    bool session_stale = s_view.session != atomic_load(&s_session);
    bool cancelled = atomic_load(&s_cancel_requested);
    if (exited || session_stale) {
        atomic_store(&s_exit_invalidated, false);
        s_view.identity_epoch = atomic_load(&s_identity_epoch);
        clear_text_locked();
        s_view.expires_at_ms = 0;
        if (s_view.worker_busy)
            publish_locked(SUITE_TRANSLATION_CANCELLED,
                           "上一任务正在清理，请稍候");
        else
            publish_locked(SUITE_TRANSLATION_IDLE, "请选择翻译");
        return invalidate;
    }
    if (cancelled && (s_view.state == SUITE_TRANSLATION_DONE ||
                      s_view.state == SUITE_TRANSLATION_FAILED ||
                      s_view.state == SUITE_TRANSLATION_WORKING)) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_CANCELLED, "已取消任务");
    }
    return invalidate;
}

/*
 * Release the snapshot lock and drain any nonblocking cancel/exit/enter intent
 * that arrived while it was held. If another owner wins the lock, that owner
 * performs the same drain on its own release. This gives offscreen cleanup a
 * bounded handoff even when no page timer or view getter runs again.
 */
static void release_view_lock(void)
{
    xSemaphoreGive(s_mutex);
    for (unsigned attempt = 0; attempt < 4U; ++attempt) {
        if (!atomic_load(&s_reconcile_required)) return;
        if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return; /* owner will drain */
        atomic_store(&s_reconcile_required, false);
        bool invalidate = reconcile_pending_locked();
        xSemaphoreGive(s_mutex);
        if (invalidate) invalidate_core_nonblocking();
    }
}

static bool session_or_cancel_stale(unsigned job_session, unsigned job_epoch)
{
    return atomic_load(&s_cancel_requested) ||
           atomic_load(&s_session) != job_session ||
           atomic_load(&s_identity_epoch) != job_epoch;
}

static void invalidate_core_blocking(void)
{
    if (!s_core_mutex) return;
    xSemaphoreTake(s_core_mutex, portMAX_DELAY);
    (void)tr_core_invalidate(&s_core, now_ms());
    xSemaphoreGive(s_core_mutex);
}

static void invalidate_core_nonblocking(void)
{
    if (!s_core_mutex) return;
    if (xSemaphoreTake(s_core_mutex, 0) != pdTRUE) return;
    (void)tr_core_invalidate(&s_core, now_ms());
    xSemaphoreGive(s_core_mutex);
}

static bool core_cancel_error(tr_error_t error)
{
    return error == TR_E_CANCELLED || error == TR_E_GENERATION ||
           error == TR_E_LATE;
}

static void write_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/*
 * Build the smallest valid canonical WAV accepted by the public core. This is
 * synthetic PCM silence, not microphone input; it lives only for one worker
 * job and is wiped and freed after cleanup.
 */
static bool make_mock_wav(uint8_t *wav)
{
    if (!wav) return false;
    const uint32_t payload = TR_WAV_MIN_DATA_BYTES;
    memset(wav, 0, TR_MOCK_WAV_BYTES);
    memcpy(wav, "RIFF", 4);
    write_u32(wav + 4, TR_MOCK_WAV_BYTES - 8U);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    write_u32(wav + 16, 16);
    write_u16(wav + 20, 1);       /* PCM */
    write_u16(wav + 22, 1);       /* mono */
    write_u32(wav + 24, 16000);   /* sample rate */
    write_u32(wav + 28, 32000);   /* byte rate */
    write_u16(wav + 32, 2);       /* block align */
    write_u16(wav + 34, 16);      /* bits */
    memcpy(wav + 36, "data", 4);
    write_u32(wav + 40, payload);
    return true;
}

static void random_operation_id(char out[TR_ID_SIZE])
{
    uint8_t bytes[16];
    static const char hex[] = "0123456789abcdef";
    esp_fill_random(bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        out[i * 2U] = hex[(bytes[i] >> 4) & 0x0fU];
        out[i * 2U + 1U] = hex[bytes[i] & 0x0fU];
    }
    out[32] = 0;
}

static size_t bounded_length(const char *text, size_t max)
{
    if (!text) return 0;
    size_t length = 0;
    while (length < max && text[length]) ++length;
    return length;
}

static void hex_encode_digest(const uint8_t digest[32], char out[TR_SHA256_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    if (!digest || !out) return;
    for (unsigned i = 0; i < 32U; ++i) {
        out[i * 2U] = hex[(digest[i] >> 4) & 0x0fU];
        out[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    out[64] = 0;
}

#ifdef SUITE_TRANSLATION_TEST
static unsigned s_test_fail_alloc_mask;
void suite_translation_test_fail_alloc(bool fail)
{
    s_test_fail_alloc_mask = fail ? 7U : 0U;
}
void suite_translation_test_fail_alloc_mask(unsigned mask)
{
    s_test_fail_alloc_mask = mask & 7U;
}
static void *worker_alloc(size_t size, unsigned bit)
{
    return (s_test_fail_alloc_mask & bit) ? NULL : malloc(size);
}
#else
static void *worker_alloc(size_t size, unsigned bit)
{
    (void)bit;
    return malloc(size);
}
#endif

static bool apply_enter_locked(unsigned session)
{
    s_view.session = session;
    s_view.identity_epoch = atomic_load(&s_identity_epoch);
    if (s_view.worker_busy) {
        atomic_store(&s_cancel_requested, true);
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_CANCELLED,
                       "上一任务正在清理，请稍候");
        return true;
    }
    atomic_store(&s_cancel_requested, false);
    bool exited = atomic_exchange(&s_exit_invalidated, false);
    if (exited) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_IDLE, "请选择翻译");
    } else {
        expire_locked(now_ms());
    }
    return false;
}

static void worker_publish_cancelled(void)
{
    invalidate_core_blocking();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clear_text_locked();
    s_view.expires_at_ms = 0;
    publish_locked(SUITE_TRANSLATION_CANCELLED, "已取消任务");
    release_view_lock();
}

static void worker_finish(unsigned job_session, unsigned job_epoch)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch) ||
        atomic_load(&s_exit_invalidated)) {
        bool session_stale = atomic_load(&s_session) != job_session ||
                             atomic_load(&s_identity_epoch) != job_epoch;
        atomic_store(&s_exit_invalidated, false);
        clear_text_locked();
        s_view.expires_at_ms = 0;
        s_view.identity_epoch = atomic_load(&s_identity_epoch);
        publish_locked(session_stale ? SUITE_TRANSLATION_IDLE
                                     : SUITE_TRANSLATION_CANCELLED,
                       session_stale ? "上一任务已取消" : "已取消任务");
    } else if (s_view.state == SUITE_TRANSLATION_WORKING) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_CANCELLED, "已取消任务");
    } else {
        s_view.serial++;
    }
    s_view.worker_busy = false;
    /* Consume an enter that could not take the snapshot lock during this
       worker's publication; otherwise its pending session would cancel the
       next job. The current atomic session/epoch is already authoritative. */
    atomic_store(&s_enter_pending, false);
    s_view.session = atomic_load(&s_session);
    s_view.identity_epoch = atomic_load(&s_identity_epoch);
    /* A cancel/exit can arrive after the check above while this lock is held.
       The next snapshot/poll reconciles it; the text is not exposed. */
    release_view_lock();
}

static void translation_mock_worker(void *arg)
{
    (void)arg;
    uint8_t *wav = NULL;
    char *source = NULL;
    char *target = NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tr_direction_t direction = s_pending_direction;
    unsigned job_session = s_pending_session;
    unsigned job_epoch = s_pending_epoch;
    release_view_lock();
    bool stale_before_admit = session_or_cancel_stale(job_session, job_epoch);
    if (stale_before_admit) goto cleanup;

    wav = worker_alloc(TR_MOCK_WAV_BYTES, 1U);
    source = worker_alloc(TR_TEXT_MAX_BYTES + 1U, 2U);
    target = worker_alloc(TR_TEXT_MAX_BYTES + 1U, 4U);
    if (!wav || !source || !target) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool stale = session_or_cancel_stale(job_session, job_epoch);
        if (!stale) {
            clear_text_locked();
            s_view.expires_at_ms = 0;
            publish_locked(SUITE_TRANSLATION_FAILED,
                           "无法准备本机数据，内存不足");
        }
        release_view_lock();
        goto cleanup;
    }
    source[0] = 0;
    target[0] = 0;
    if (!make_mock_wav(wav)) goto cleanup;

    char operation_id[TR_ID_SIZE];
    char digest[TR_SHA256_SIZE];
    random_operation_id(operation_id);
    /* Hash the synthetic clip outside both locks. */
    if (!tr_sha256_hex(wav, TR_MOCK_WAV_BYTES, digest)) goto cleanup;
    if (session_or_cancel_stale(job_session, job_epoch)) goto cleanup;

    tr_job_t admitted;
    tr_error_t error;
    xSemaphoreTake(s_core_mutex, portMAX_DELAY);
    tr_admission_t request = {
        .operation_id = operation_id,
        .direction = direction,
        .audio_sha256 = digest,
        .audio_bytes = TR_MOCK_WAV_BYTES,
    };
    error = tr_core_admit(&s_core, &s_capability, &request, now_ms(), &admitted);
    xSemaphoreGive(s_core_mutex);

    if (session_or_cancel_stale(job_session, job_epoch)) {
        if (error == TR_OK) {
            worker_publish_cancelled();
        }
        goto cleanup;
    }
    if (error == TR_E_BUSY) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_BUSY,
                       "本机记录已满（8/8），可清除本机记录");
        release_view_lock();
        goto cleanup;
    }
    if (error != TR_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_FAILED, "记录写入失败");
        ESP_LOGW(TAG, "admit failed: %s", tr_error_name(error));
        release_view_lock();
        goto cleanup;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch)) {
        release_view_lock();
        worker_publish_cancelled();
        goto cleanup;
    }
    s_view.core_generation = admitted.generation;
    s_view.expires_at_ms = 0;
    clear_text_locked();
    publish_locked(SUITE_TRANSLATION_WORKING, "正在本机识别翻译");
    release_view_lock();

    xSemaphoreTake(s_core_mutex, portMAX_DELAY);
    tr_job_t current;
    error = tr_core_get(&s_core, operation_id, &current);
    if (error == TR_OK) {
        error = tr_core_upload(&s_core, &s_capability, operation_id,
                               current.job_id, current.generation, wav,
                               TR_MOCK_WAV_BYTES, now_ms());
    }
    if (error == TR_OK) {
        error = tr_core_running(&s_core, &s_capability, operation_id,
                                current.job_id, current.generation, now_ms());
    }
    xSemaphoreGive(s_core_mutex);

    if (error != TR_OK) {
        if (core_cancel_error(error) ||
            session_or_cancel_stale(job_session, job_epoch)) {
            worker_publish_cancelled();
        } else {
            invalidate_core_blocking();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            clear_text_locked();
            s_view.expires_at_ms = 0;
            publish_locked(SUITE_TRANSLATION_FAILED, "语音校验失败");
            ESP_LOGW(TAG, "upload/running failed: %s", tr_error_name(error));
            release_view_lock();
        }
        goto cleanup;
    }
    if (session_or_cancel_stale(job_session, job_epoch)) {
        worker_publish_cancelled();
        goto cleanup;
    }

    const suite_translation_provider_t *provider = s_provider;
    suite_translation_provider_result_t result = SUITE_TR_PROVIDER_RECOGNIZE_FAILED;
    if (provider && provider->run) {
        result = provider->run(direction, source, TR_TEXT_MAX_BYTES + 1U,
                               target, TR_TEXT_MAX_BYTES + 1U);
    }

    /*
     * Provider output must be terminated inside the supplied cap. Do not write
     * a terminator here: that would silently accept an unterminated 2049-byte
     * buffer. Exact byte lengths are passed to the core and view unchanged.
     */
    const size_t cap = TR_TEXT_MAX_BYTES + 1U;
    size_t source_length = bounded_length(source, cap);
    size_t target_length = bounded_length(target, cap);
    bool source_terminated = source_length < cap;
    bool target_terminated = target_length < cap;
    bool output_ok = result == SUITE_TR_PROVIDER_OK && source_terminated &&
                     target_terminated && source_length && target_length;
    bool partial_ok = result == SUITE_TR_PROVIDER_TRANSLATE_FAILED &&
                      source_terminated && source_length;

    if (session_or_cancel_stale(job_session, job_epoch)) {
        worker_publish_cancelled();
        goto cleanup;
    }

    xSemaphoreTake(s_core_mutex, portMAX_DELAY);
    error = tr_core_get(&s_core, operation_id, &current);
    if (error == TR_OK) {
        if (output_ok) {
            error = tr_core_result(&s_core, &s_capability, operation_id,
                                   current.job_id, current.generation, source,
                                   source_length, target, target_length,
                                   TR_STATE_DONE, now_ms());
        } else if (partial_ok) {
            error = tr_core_result(&s_core, &s_capability, operation_id,
                                   current.job_id, current.generation, source,
                                   source_length, NULL, 0, TR_STATE_FAILED,
                                   now_ms());
        } else {
            error = tr_core_result(&s_core, &s_capability, operation_id,
                                   current.job_id, current.generation, NULL, 0,
                                   NULL, 0, TR_STATE_FAILED, now_ms());
        }
    }
    xSemaphoreGive(s_core_mutex);

    if (error != TR_OK) {
        if (core_cancel_error(error)) {
            worker_publish_cancelled();
        } else {
            invalidate_core_blocking();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            clear_text_locked();
            s_view.expires_at_ms = 0;
            publish_locked(SUITE_TRANSLATION_FAILED, "结果未通过校验");
            release_view_lock();
        }
        goto cleanup;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch)) {
        release_view_lock();
        worker_publish_cancelled();
        goto cleanup;
    }
    clear_text_locked();
    s_view.expires_at_ms = 0;
    if (output_ok) {
        snprintf(s_view.source, sizeof(s_view.source), "%s", source);
        snprintf(s_view.target, sizeof(s_view.target), "%s", target);
        s_view.expires_at_ms = now_ms() + SUITE_TRANSLATION_TTL_MS;
        publish_locked(SUITE_TRANSLATION_DONE, "本机翻译完成");
    } else if (partial_ok) {
        snprintf(s_view.source, sizeof(s_view.source), "%s", source);
        s_view.expires_at_ms = now_ms() + SUITE_TRANSLATION_TTL_MS;
        publish_locked(SUITE_TRANSLATION_FAILED, "翻译失败，源文已保留");
    } else if (result == SUITE_TR_PROVIDER_OK) {
        publish_locked(SUITE_TRANSLATION_FAILED, "结果未通过校验");
    } else {
        publish_locked(SUITE_TRANSLATION_FAILED, "识别失败，请重试");
    }
    release_view_lock();

cleanup:
    if (wav) {
        wipe_memory(wav, TR_MOCK_WAV_BYTES);
        free(wav);
    }
    if (source) {
        wipe_memory(source, TR_TEXT_MAX_BYTES + 1U);
        free(source);
    }
    if (target) {
        wipe_memory(target, TR_TEXT_MAX_BYTES + 1U);
        free(target);
    }
    worker_finish(job_session, job_epoch);
    vTaskDelete(NULL);
}


#define TR_REAL_STACK 8192U
#define TR_REAL_READY_US (15LL * 1000 * 1000)
#define TR_REAL_CAPTURE_US (12LL * 1000 * 1000)
#define TR_REAL_TOTAL_US (60LL * 1000 * 1000)
#define TR_REAL_CANCEL_US (2LL * 1000 * 1000)
#define TR_REAL_ACQUIRE_MS 2000U
#define TR_REAL_POLL_MS 500U

typedef struct {
    unsigned session, epoch;
} real_cancel_t;

static bool real_cancelled(void *ctx)
{
    real_cancel_t *cancel = ctx;
    return session_or_cancel_stale(cancel->session, cancel->epoch) ||
           suite_translation_usb_window_open();
}

static void real_diag(const char *stage, unsigned result, unsigned bytes)
{
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG,
             "TRDIAG stage=%s result=%u bytes=%u free=%u largest=%u "
             "dma_free=%u dma_large=%u stack=%u t_us=%lld elapsed_us=%lld",
             stage, result, bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (long long)now, (long long)(now - s_diag_start_us));
}

static void real_publish(suite_translation_state_t state, const char *status)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clear_text_locked();
    s_view.expires_at_ms = 0;
    publish_locked(state, status);
    release_view_lock();
}

/*
 * Add a compact ASCII numeric line under the unchanged Chinese first line.
 * Allocation failures use A/F/L; readiness failures use R/H/B. Allocation
 * samples are taken once, directly after the first failed allocation and before
 * later allocations are attempted; they are near-failure samples under
 * concurrent background tasks, not an atomic heap proof. Cancel/exit/session
 * invalidation wins: worker_finish publishes the terminal cancellation/idle
 * state instead of this stale failure.
 */
static void real_publish_numeric_failure(
    const char *text, unsigned job_session, unsigned job_epoch,
    const suite_translation_tls_diag_t *tls_diag, const char *detail_format,
    unsigned a, unsigned b, unsigned c)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch) ||
        atomic_load(&s_exit_invalidated)) {
        release_view_lock();
        return;
    }
    clear_text_locked();
    s_view.expires_at_ms = 0;
    int written = snprintf(s_view.status, sizeof(s_view.status), "%s\n", text);
    if (written > 0 && (size_t)written < sizeof(s_view.status)) {
        if (tls_diag && tls_diag->valid) {
            snprintf(s_view.status + written,
                     sizeof(s_view.status) - (size_t)written,
                     "R%u S%u X%d", a, (unsigned)tls_diag->stage,
                     (int)tls_diag->local_errno);
            s_view.tls_diag = *tls_diag;
        } else {
            snprintf(s_view.status + written,
                     sizeof(s_view.status) - (size_t)written,
                     detail_format, a, b, c);
        }
    }
    s_view.state = SUITE_TRANSLATION_FAILED;
    s_view.serial++;
    release_view_lock();
}

static void alloc_memory_sample(unsigned *free_bytes, unsigned *largest_bytes)
{
    *free_bytes = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT);
    *largest_bytes = (unsigned)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static size_t real_audio_read(void *ctx, uint32_t offset, uint8_t *buffer,
                              size_t capacity)
{
    suite_audio_lease_t *lease = ctx;
    if (!lease || !buffer || !capacity) return 0;
    uint32_t total = suite_audio_job_wav_bytes(lease);
    if (offset >= total) return 0;
    size_t n = capacity;
    if (n > SUITE_TR_HTTP_AUDIO_CHUNK) n = SUITE_TR_HTTP_AUDIO_CHUNK;
    if (n > total - offset) n = total - offset;
    return suite_audio_job_read(lease, offset, buffer, n) == ESP_OK ? n : 0;
}

static suite_translation_protocol_direction_t protocol_direction(
    tr_direction_t direction)
{
    return direction == TR_DIRECTION_ZH_EN
               ? SUITE_TR_PROTOCOL_DIRECTION_ZH_EN
               : SUITE_TR_PROTOCOL_DIRECTION_EN_ZH;
}

static bool protocol_to_direction(suite_translation_protocol_direction_t value,
                                  tr_direction_t *out)
{
    if (value == SUITE_TR_PROTOCOL_DIRECTION_EN_ZH) {
        *out = TR_DIRECTION_EN_ZH;
        return true;
    }
    if (value == SUITE_TR_PROTOCOL_DIRECTION_ZH_EN) {
        *out = TR_DIRECTION_ZH_EN;
        return true;
    }
    return false;
}

static bool text_copy_bounded(char *dst, size_t cap, const char *src,
                              size_t length)
{
    if (!dst || !cap || !src || length >= cap) return false;
    memcpy(dst, src, length);
    dst[length] = 0;
    return true;
}

static void translation_real_worker(void *arg)
{
    (void)arg;
    suite_translation_config_t config;
    memset(&config, 0, sizeof(config));
    suite_audio_lease_t *lease = NULL;
    char *text_source = NULL;
    char *text_target = NULL;
    uint8_t digest[32];
    char sha[TR_SHA256_SIZE];
    char operation_id[TR_ID_SIZE];
    char job_id[TR_ID_SIZE] = {0};
    suite_translation_protocol_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    memset(digest, 0, sizeof(digest));
    memset(sha, 0, sizeof(sha));
    memset(operation_id, 0, sizeof(operation_id));
    suite_translation_protocol_view_t parsed;
    bool job_known = false;
    bool cancelled = false;
    bool unknown = false;
    bool cleanup_failed = false;
    bool audio_attempted = false;
    bool audio_released = false;
    bool audio_release_failed = false;
    uint8_t pcm[SUITE_AUDIO_CHUNK_MAX];
    const char *failure = NULL;
    const char *unknown_status = "服务结果未知，请稍后";
    suite_translation_state_t failure_state = SUITE_TRANSLATION_FAILED;
    unsigned failure_result = 0;
    unsigned alloc_mask = 0;
    unsigned alloc_free = 0;
    unsigned alloc_largest = 0;
    unsigned ready_result = 0;
    unsigned ready_http_status = 0;
    unsigned ready_flag = 0;
    uint32_t pcm_total = 0;
    tr_direction_t direction;
    unsigned job_session, job_epoch;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    direction = s_pending_direction;
    job_session = s_pending_session;
    job_epoch = s_pending_epoch;
    release_view_lock();
    real_cancel_t cancel_ctx = { .session = job_session, .epoch = job_epoch };
    s_diag_start_us = esp_timer_get_time();

    if (real_cancelled(&cancel_ctx)) {
        cancelled = true;
        goto finish;
    }
    if (suite_translation_usb_window_open()) {
        failure = "USB配置中，请稍后";
        failure_state = SUITE_TRANSLATION_UNAVAILABLE;
        failure_result = 1;
        goto finish;
    }
    if (suite_connection_state() != SUITE_CONN_ONLINE) {
        failure = "离线，无法翻译";
        failure_result = 2;
        goto finish;
    }
    if (!suite_time_is_trusted(suite_connection_time(), suite_connection_now())) {
        failure = "请先联网校时";
        failure_result = 3;
        goto finish;
    }
    if (suite_translation_config_get(&config) != SUITE_TRANSLATION_CONFIG_OK) {
        failure = "翻译未配置，请用USB配置";
        failure_state = SUITE_TRANSLATION_UNAVAILABLE;
        failure_result = 4;
        goto finish;
    }
    if (config.schema != SUITE_TRANSLATION_CONFIG_SCHEMA) {
        failure = "翻译配置无效，请重新配置";
        failure_state = SUITE_TRANSLATION_UNAVAILABLE;
        failure_result = 5;
        goto finish;
    }

    suite_translation_http_t http;
    if (suite_translation_http_init(&http, config.origin, config.token, NULL,
                                    SUITE_TR_HTTP_JSON_CAP) != SUITE_TR_HTTP_OK) {
        failure = "翻译网络配置无效";
        failure_result = 7;
        goto finish;
    }

    suite_translation_http_info_t info;
    memset(&info, 0, sizeof(info));
    suite_translation_protocol_ready_t ready;
    memset(&ready, 0, sizeof(ready));
    suite_translation_http_call_t call = {
        .deadline_us = esp_timer_get_time() + TR_REAL_READY_US,
        .cancel = real_cancelled,
        .cancel_ctx = &cancel_ctx,
    };
    suite_translation_http_result_t result =
        suite_translation_http_ready(&http, &call, &ready, &info);
    ready_result = (unsigned)result;
    ready_http_status = info.http_status > 0 ? (unsigned)info.http_status : 0U;
    ready_flag = ready.ready ? 1U : 0U;
    real_diag("preflight", (unsigned)result, 0);
    if (result == SUITE_TR_HTTP_E_BUFFER) {
        failure = "无法准备翻译缓存，内存不足";
        failure_result = 30;
        goto finish;
    }
    if (result != SUITE_TR_HTTP_OK || !ready.ready) {
        failure = "翻译服务不可用";
        failure_result = 8;
        goto finish;
    }
    if (real_cancelled(&cancel_ctx)) {
        cancelled = true;
        goto finish;
    }
    if (suite_translation_usb_window_open()) {
        failure = "USB配置中，请稍后";
        failure_state = SUITE_TRANSLATION_UNAVAILABLE;
        failure_result = 9;
        goto finish;
    }

    bool alloc_sampled = false;
    text_source = worker_alloc(TR_TEXT_MAX_BYTES + 1U, 2U);
    if (!text_source) {
        alloc_mask |= 2U;
        alloc_memory_sample(&alloc_free, &alloc_largest);
        alloc_sampled = true;
    }
    text_target = worker_alloc(TR_TEXT_MAX_BYTES + 1U, 4U);
    if (!text_target) {
        alloc_mask |= 4U;
        if (!alloc_sampled) {
            alloc_memory_sample(&alloc_free, &alloc_largest);
            alloc_sampled = true;
        }
    }
    if (alloc_mask) {
        failure = "无法准备翻译缓存，内存不足";
        failure_result = 6;
        goto finish;
    }
    text_source[0] = 0;
    text_target[0] = 0;
    suite_translation_protocol_view_init(&parsed, text_source,
                                         TR_TEXT_MAX_BYTES + 1U, text_target,
                                         TR_TEXT_MAX_BYTES + 1U);

    esp_err_t audio = suite_audio_job_acquire_ex(
        SUITE_AUDIO_OWNER_TRANSLATION, TR_REAL_ACQUIRE_MS, real_cancelled,
        &cancel_ctx, &lease);
    if (audio != ESP_OK) {
        if (real_cancelled(&cancel_ctx)) {
            cancelled = true;
        } else {
            failure = "录音资源忙，请稍后再试";
            failure_result = 10;
        }
        goto finish;
    }
    audio = suite_audio_job_stage_begin(lease, SUITE_AUDIO_TRANSLATION_SPAN);
    if (audio != ESP_OK) {
        failure = "录音保存失败";
        failure_result = 11;
        goto finish;
    }
    audio_attempted = true;
    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        failure = "录音启动失败";
        failure_result = 12;
        goto finish;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch)) {
        release_view_lock();
        cancelled = true;
        goto finish;
    }
    clear_text_locked();
    s_view.expires_at_ms = 0;
    s_view.capture_seconds = 0;
    publish_locked(SUITE_TRANSLATION_RECORDING,
                   "正在录音 OK结束 UP取消");
    release_view_lock();

    real_diag("capture_started", 0, 0);
    const uint32_t pcm_max = suite_audio_job_pcm_max(lease);
    const int64_t capture_deadline = esp_timer_get_time() + TR_REAL_CAPTURE_US;
    bool capture_finished = false;
    bool capture_failed = false;
    while (pcm_total < pcm_max) {
        if (real_cancelled(&cancel_ctx)) {
            cancelled = true;
            break;
        }
        if (atomic_load(&s_capture_finish)) {
            capture_finished = true;
            break;
        }
        if (esp_timer_get_time() >= capture_deadline) {
            capture_finished = true;
            break;
        }
        size_t chunk = pcm_max - pcm_total;
        if (chunk > sizeof(pcm)) chunk = sizeof(pcm);
        if (bsp_audio_read(pcm, chunk) != ESP_OK) {
            capture_failed = true;
            break;
        }
        if (suite_audio_job_append(lease, pcm, chunk) != ESP_OK) {
            capture_failed = true;
            break;
        }
        pcm_total += (uint32_t)chunk;
        uint32_t seconds = pcm_total / 32000U;
        if (seconds) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_view.capture_seconds != seconds) {
                s_view.capture_seconds = seconds;
                s_view.serial++;
            }
            release_view_lock();
        }
    }
    if (pcm_total >= pcm_max) capture_finished = true;
    if (!capture_failed && !cancelled && !capture_finished) {
        failure = "录音时间过长";
        failure_result = 13;
    }
    int64_t total_deadline = esp_timer_get_time() + TR_REAL_TOTAL_US;
    esp_err_t released = bsp_audio_release();
    audio_released = true;
    if (released != ESP_OK) {
        audio_release_failed = true;
        failure = "录音结束失败";
        failure_result = 14;
    }
    real_diag("audio_released", audio_release_failed ? 1U : 0U, pcm_total);
    real_diag("capture", cancelled ? 1U : (capture_failed ? 2U : 0U),
              pcm_total);
    if (cancelled) goto finish;
    if (audio_release_failed) goto finish;
    if (capture_failed) {
        failure = "录音失败";
        failure_result = 15;
        goto finish;
    }
    if (pcm_total < TR_WAV_MIN_DATA_BYTES) {
        failure = "录音不足，请重试";
        failure_result = 16;
        goto finish;
    }
    if (suite_audio_job_finalize(lease) != ESP_OK) {
        failure = "录音保存失败";
        failure_result = 17;
        goto finish;
    }
    uint32_t wav_bytes = suite_audio_job_wav_bytes(lease);
    if (wav_bytes < SUITE_TR_PROTOCOL_AUDIO_MIN || wav_bytes > TR_WAV_MAX_BYTES) {
        failure = "录音大小无效";
        failure_result = 18;
        goto finish;
    }
    if (suite_audio_job_sha256(lease, digest) != ESP_OK) {
        failure = "录音校验失败";
        failure_result = 19;
        goto finish;
    }
    hex_encode_digest(digest, sha);
    random_operation_id(operation_id);

    /* Publish the operation identity before admitting. If the POST outcome
       becomes ambiguous, the view still identifies which operation was
       attempted; no job exists yet, so no stale job identity is retained. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (session_or_cancel_stale(job_session, job_epoch) ||
        atomic_load(&s_exit_invalidated)) {
        release_view_lock();
        cancelled = true;
        goto finish;
    }
    snprintf(s_view.operation_id, sizeof(s_view.operation_id), "%s",
             operation_id);
    s_view.job_id[0] = 0;
    s_view.server_generation = 0;
    s_view.serial++;
    release_view_lock();

    call.deadline_us = total_deadline;
    memset(&info, 0, sizeof(info));
    result = suite_translation_http_admit(
        &http, &call, operation_id, protocol_direction(direction), sha,
        wav_bytes, &parsed, &info);
    real_diag("admit", (unsigned)result, wav_bytes);
    if (result != SUITE_TR_HTTP_OK) {
        if (real_cancelled(&cancel_ctx)) {
            cancelled = true;
        } else if (result == SUITE_TR_HTTP_E_BUFFER && !info.ambiguous) {
            failure = "无法准备翻译缓存，内存不足";
            failure_result = 30;
        } else if (info.ambiguous) {
            /* Request bytes may have reached the server: keep the outcome
               unknown, and keep the OOM cause visible when that was the late
               failure. Never replay the POST or issue an implicit DELETE. */
            unknown = true;
            if (result == SUITE_TR_HTTP_E_BUFFER)
                unknown_status = "内存不足，服务结果未知";
        } else {
            failure = "翻译请求失败";
            failure_result = 20;
        }
        goto finish;
    }
    if (!suite_translation_protocol_id_valid(parsed.job_id) ||
        !suite_translation_protocol_id_valid(parsed.operation_id) ||
        strcmp(parsed.operation_id, operation_id) ||
        parsed.audio_bytes != wav_bytes ||
        !protocol_to_direction(parsed.direction, &direction)) {
        failure = "服务结果异常";
        failure_result = 21;
        goto finish;
    }
    strcpy(job_id, parsed.job_id);
    identity.operation_id = operation_id;
    identity.job_id = job_id;
    identity.generation = parsed.generation;
    identity.direction = protocol_direction(direction);
    identity.audio_bytes = wav_bytes;
    job_known = true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_view.operation_id, sizeof(s_view.operation_id), "%s",
             operation_id);
    snprintf(s_view.job_id, sizeof(s_view.job_id), "%s", job_id);
    s_view.server_generation = parsed.generation;
    publish_locked(SUITE_TRANSLATION_WORKING, "正在上传录音");
    release_view_lock();
    if (real_cancelled(&cancel_ctx)) {
        cancelled = true;
        goto finish;
    }

    call.deadline_us = total_deadline;
    memset(&info, 0, sizeof(info));
    result = suite_translation_http_upload(&http, &call, &identity,
                                           real_audio_read, lease, &parsed,
                                           &info);
    real_diag("upload", (unsigned)result, wav_bytes);
    if (real_cancelled(&cancel_ctx)) {
        cancelled = true;
        goto finish;
    }
    if (result != SUITE_TR_HTTP_OK) {
        if (result == SUITE_TR_HTTP_E_BUFFER && !info.ambiguous) {
            failure = "无法准备翻译缓存，内存不足";
            failure_result = 30;
        } else if (info.ambiguous) {
            unknown = true;
            if (result == SUITE_TR_HTTP_E_BUFFER)
                unknown_status = "内存不足，服务结果未知";
        } else {
            failure = "录音上传失败";
            failure_result = 22;
        }
        goto finish;
    }
    if (suite_audio_job_release(lease) != ESP_OK) {
        lease = NULL;
        cleanup_failed = true;
        failure = "录音清理失败，请重启";
        failure_state = SUITE_TRANSLATION_FAILED;
        failure_result = 23;
        goto finish;
    }
    lease = NULL;
    real_diag("cleanup", 0, wav_bytes);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    publish_locked(SUITE_TRANSLATION_WORKING, "正在等待翻译结果");
    release_view_lock();

    for (;;) {
        if (real_cancelled(&cancel_ctx)) {
            cancelled = true;
            break;
        }
        if (esp_timer_get_time() >= total_deadline) {
            unknown = true;
            failure_result = 24;
            break;
        }
        call.deadline_us = total_deadline;
        memset(&info, 0, sizeof(info));
        result = suite_translation_http_status(&http, &call, &identity,
                                               &parsed, &info);
        real_diag("status", (unsigned)result, parsed.target_length);
        if (real_cancelled(&cancel_ctx)) {
            cancelled = true;
            break;
        }
        if (result != SUITE_TR_HTTP_OK) {
            /* EXPIRED/AUDIO_EXPIRED are not yet in the shared protocol enum;
               a protocol parse failure is reported as unknown, never retried. */
            if (result == SUITE_TR_HTTP_E_BUFFER && !info.ambiguous) {
                failure = "无法准备翻译缓存，内存不足";
                failure_result = 30;
            } else if (info.ambiguous || result == SUITE_TR_HTTP_E_DEADLINE ||
                       result == SUITE_TR_HTTP_E_PROTOCOL) {
                unknown = true;
                if (result == SUITE_TR_HTTP_E_BUFFER)
                    unknown_status = "内存不足，服务结果未知";
            } else {
                failure = "翻译结果失败";
                failure_result = 25;
            }
            break;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_DONE) {
            if (!parsed.has_source || !parsed.has_target) {
                failure = "翻译结果无效";
                failure_result = 26;
                break;
            }
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (session_or_cancel_stale(job_session, job_epoch)) {
                release_view_lock();
                cancelled = true;
                break;
            }
            clear_text_locked();
            if (!text_copy_bounded(s_view.source, sizeof(s_view.source),
                                   text_source, parsed.source_length) ||
                !text_copy_bounded(s_view.target, sizeof(s_view.target),
                                   text_target, parsed.target_length)) {
                release_view_lock();
                failure = "翻译结果无效";
                failure_result = 27;
                break;
            }
            s_view.expires_at_ms = now_ms() + SUITE_TRANSLATION_TTL_MS;
            publish_locked(SUITE_TRANSLATION_DONE, "翻译完成");
            release_view_lock();
            goto finish;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_EXPIRED ||
            (parsed.state == SUITE_TR_PROTOCOL_STATE_FAILED &&
             parsed.has_error && !strcmp(parsed.error, "AUDIO_EXPIRED"))) {
            real_publish(SUITE_TRANSLATION_EXPIRED,
                         "结果已过期，请重新翻译");
            goto finish;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_FAILED) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (session_or_cancel_stale(job_session, job_epoch)) {
                release_view_lock();
                cancelled = true;
                break;
            }
            clear_text_locked();
            s_view.expires_at_ms = 0;
            if (parsed.has_source &&
                text_copy_bounded(s_view.source, sizeof(s_view.source),
                                  text_source, parsed.source_length)) {
                s_view.expires_at_ms = now_ms() + SUITE_TRANSLATION_TTL_MS;
                publish_locked(SUITE_TRANSLATION_FAILED,
                               "翻译失败，源文已保留");
            } else {
                publish_locked(SUITE_TRANSLATION_FAILED, "翻译失败");
            }
            release_view_lock();
            goto finish;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_CANCELLED) {
            real_publish(SUITE_TRANSLATION_CANCELLED, "已取消任务");
            goto finish;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_TEXT_LOST) {
            real_publish(SUITE_TRANSLATION_EXPIRED,
                         "结果已过期，请重新翻译");
            goto finish;
        }
        if (parsed.state == SUITE_TR_PROTOCOL_STATE_REMOTE_UNKNOWN ||
            parsed.state == SUITE_TR_PROTOCOL_STATE_UNKNOWN) {
            real_publish(SUITE_TRANSLATION_UNKNOWN,
                         "服务结果未知，请稍后");
            goto finish;
        }
        if (parsed.state != SUITE_TR_PROTOCOL_STATE_ADMITTED &&
            parsed.state != SUITE_TR_PROTOCOL_STATE_RUNNING) {
            /* Forward-compatible handling for a state not yet represented in
               this protocol header (for example EXPIRED). */
            real_publish(SUITE_TRANSLATION_UNKNOWN,
                         "服务结果未知，请稍后");
            goto finish;
        }
        vTaskDelay(pdMS_TO_TICKS(TR_REAL_POLL_MS));
    }

finish:
    if (real_cancelled(&cancel_ctx)) cancelled = true;
    if (audio_attempted && !audio_released) {
        audio_released = true;
        if (bsp_audio_release() != ESP_OK) {
            audio_release_failed = true;
            failure = "录音结束失败";
            failure_result = 29;
        }
    }
    if (lease) {
        esp_err_t cleanup = suite_audio_job_release(lease);
        lease = NULL;
        real_diag("cleanup", (unsigned)cleanup, 0);
        if (cleanup != ESP_OK) {
            cleanup_failed = true;
            failure = "录音清理失败，请重启";
            failure_state = SUITE_TRANSLATION_FAILED;
            failure_result = 28;
        }
    } else if (!cancelled && !unknown && !failure) {
        real_diag("final", 0, 0);
    }
    if (!cancelled && real_cancelled(&cancel_ctx)) cancelled = true;
    if (cancelled && job_known) {
        suite_translation_http_call_t cancel_call = {
            .deadline_us = esp_timer_get_time() + TR_REAL_CANCEL_US,
            .cancel = NULL,
            .cancel_ctx = NULL,
        };
        suite_translation_http_info_t cancel_info;
        memset(&cancel_info, 0, sizeof(cancel_info));
        (void)suite_translation_http_cancel(&http, &cancel_call, &identity,
                                            &parsed, &cancel_info);
    }
    if (cleanup_failed) {
        real_publish(SUITE_TRANSLATION_FAILED, "录音清理失败，请重启");
    } else if (audio_release_failed) {
        real_publish(SUITE_TRANSLATION_FAILED, "录音结束失败");
    } else if (cancelled) {
        real_publish(SUITE_TRANSLATION_CANCELLED, "已取消任务");
    } else if (unknown) {
        real_publish(SUITE_TRANSLATION_UNKNOWN, unknown_status);
    } else if (failure) {
        real_diag("failed", failure_result, 0);
        if (failure_result == 6U) {
            real_publish_numeric_failure(failure, job_session, job_epoch,
                                         NULL, "A%u F%u L%u", alloc_mask,
                                         alloc_free, alloc_largest);
        } else if (failure_result == 8U) {
            const suite_translation_tls_diag_t *diag =
                info.tls_diag.valid ? &info.tls_diag : NULL;
            real_publish_numeric_failure(failure, job_session, job_epoch,
                                         diag, "R%u H%u B%u", ready_result,
                                         ready_http_status, ready_flag);
        } else {
            real_publish(failure_state, failure);
        }
    }

    mbedtls_platform_zeroize(&config, sizeof(config));
    if (text_source) {
        wipe_memory(text_source, TR_TEXT_MAX_BYTES + 1U);
        free(text_source);
    }
    if (text_target) {
        wipe_memory(text_target, TR_TEXT_MAX_BYTES + 1U);
        free(text_target);
    }
    wipe_memory(pcm, sizeof(pcm));
    wipe_memory(digest, sizeof(digest));
    wipe_memory(sha, sizeof(sha));
    wipe_memory(operation_id, sizeof(operation_id));
    wipe_memory(job_id, sizeof(job_id));
    worker_finish(job_session, job_epoch);
    vTaskDelete(NULL);
}

static void translation_worker(void *arg)
{
    (void)arg;
    if (s_pending_mode == SUITE_TRANSLATION_MODE_REAL)
        translation_real_worker(arg);
    else
        translation_mock_worker(arg);
}

esp_err_t suite_translation_start(void)
{
    if (s_started) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_core_mutex = xSemaphoreCreateMutex();
    if (!s_core_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&s_view, 0, sizeof(s_view));
    s_view.state = SUITE_TRANSLATION_IDLE;
    s_view.direction = TR_DIRECTION_EN_ZH;
    s_view.mode = SUITE_TRANSLATION_MODE_MOCK;
    s_view.mock = true;
    s_view.capture_seconds = 0;
    s_view.operation_id[0] = 0;
    s_view.job_id[0] = 0;
    s_view.server_generation = 0;
    s_view.session = atomic_load(&s_session);
    s_view.identity_epoch = atomic_load(&s_identity_epoch);
    atomic_store(&s_reconcile_required, false);
    set_status_locked("请选择翻译");
    tr_core_init(&s_core);
    if (tr_capability_init(&s_capability, TR_SCOPE_TRANSLATION, TR_MOCK_TOKEN,
                           strlen(TR_MOCK_TOKEN)) != TR_OK) {
        vSemaphoreDelete(s_core_mutex);
        vSemaphoreDelete(s_mutex);
        s_core_mutex = NULL;
        s_mutex = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;
    return ESP_OK;
}

void suite_translation_session_enter(void)
{
    if (!s_started || !s_mutex) return;
    unsigned session = atomic_fetch_add(&s_session, 1U) + 1U;
    atomic_store(&s_reconcile_required, true);
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_store(&s_enter_session, session);
        atomic_store(&s_enter_pending, true);
        return;
    }
    atomic_store(&s_enter_pending, false);
    bool invalidate = apply_enter_locked(session);
    release_view_lock();
    if (invalidate) invalidate_core_nonblocking();
}

void suite_translation_session_exit(void)
{
    if (!s_started || !s_mutex) return;
    unsigned session = atomic_fetch_add(&s_session, 1U) + 1U;
    atomic_store(&s_reconcile_required, true);
    atomic_store(&s_exit_invalidated, true);
    atomic_store(&s_cancel_requested, true);
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    s_view.session = session;
    bool invalidate = false;
    if (s_view.worker_busy) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_CANCELLED,
                       "已离开，旧结果不保留");
        invalidate = true;
    } else {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_IDLE, "请选择翻译");
    }
    release_view_lock();
    if (invalidate) invalidate_core_nonblocking();
}

bool suite_translation_select(tr_direction_t direction)
{
    if (!s_started || !s_mutex) return false;
    if (direction != TR_DIRECTION_EN_ZH &&
        direction != TR_DIRECTION_ZH_EN)
        return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    bool accepted = !s_view.worker_busy &&
                    s_view.state != SUITE_TRANSLATION_WORKING &&
                    s_view.state != SUITE_TRANSLATION_RECORDING;
    if (accepted) {
        s_view.direction = direction;
        s_view.serial++;
    }
    release_view_lock();
    return accepted;
}

bool suite_translation_select_mode(suite_translation_mode_t mode)
{
    if (!s_started || !s_mutex) return false;
    if (mode != SUITE_TRANSLATION_MODE_MOCK &&
        mode != SUITE_TRANSLATION_MODE_REAL)
        return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    bool accepted = !s_view.worker_busy &&
                    s_view.state != SUITE_TRANSLATION_WORKING &&
                    s_view.state != SUITE_TRANSLATION_RECORDING;
    if (accepted) {
        s_view.mode = mode;
        s_view.mock = mode == SUITE_TRANSLATION_MODE_MOCK;
        s_view.serial++;
    }
    release_view_lock();
    return accepted;
}

void suite_translation_finish_recording(void)
{
    atomic_store(&s_capture_finish, true);
}

bool suite_translation_busy(void)
{
    if (!s_started || !s_mutex) return true;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return true;
    bool busy = s_view.worker_busy;
    release_view_lock();
    return busy;
}

bool suite_translation_run(void)
{
    if (!s_started || !s_mutex) return false;
    if (suite_translation_usb_window_open()) {
        if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
            set_status_locked("USB配置中，请稍后");
            s_view.serial++;
            release_view_lock();
        }
        return false;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    if (s_view.worker_busy || s_view.state == SUITE_TRANSLATION_WORKING ||
        s_view.state == SUITE_TRANSLATION_RECORDING) {
        set_status_locked("上一任务未清理，请稍候");
        s_view.serial++;
        release_view_lock();
        return false;
    }
    atomic_store(&s_dispatch_failed, false);
    atomic_store(&s_capture_finish, false);
    s_pending_direction = s_view.direction;
    s_pending_mode = s_view.mode;
    s_pending_session = atomic_load(&s_session);
    s_pending_epoch = atomic_load(&s_identity_epoch);
    s_view.session = s_pending_session;
    s_view.identity_epoch = s_pending_epoch;
    s_view.capture_seconds = 0;
    s_view.operation_id[0] = 0;
    s_view.job_id[0] = 0;
    s_view.server_generation = 0;
    atomic_store(&s_cancel_requested, false);
    s_view.expires_at_ms = 0;
    clear_text_locked();
    s_view.worker_busy = true;
    publish_locked(SUITE_TRANSLATION_WORKING,
                   s_pending_mode == SUITE_TRANSLATION_MODE_REAL
                       ? "正在准备联网翻译"
                       : "正在准备本机输入");
    release_view_lock();

    if (xTaskCreate(translation_worker, "suite_tr", TR_WORKER_STACK, NULL,
                    TR_WORKER_PRIORITY, NULL) != pdPASS) {
        if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
            s_view.worker_busy = false;
            clear_text_locked();
            s_view.expires_at_ms = 0;
            publish_locked(SUITE_TRANSLATION_UNAVAILABLE,
                           "无法创建任务，内存不足");
            release_view_lock();
        } else {
            atomic_store(&s_dispatch_failed, true);
            atomic_store(&s_reconcile_required, true);
        }
        return false;
    }
    return true;
}

void suite_translation_cancel(void)
{
    if (!s_started || !s_mutex) return;
    atomic_store(&s_reconcile_required, true);
    atomic_store(&s_cancel_requested, true);
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    bool invalidate = false;
    if (s_view.worker_busy || s_view.state == SUITE_TRANSLATION_WORKING ||
        s_view.state == SUITE_TRANSLATION_RECORDING) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_CANCELLED, "正在取消任务");
        invalidate = s_pending_mode != SUITE_TRANSLATION_MODE_REAL;
    }
    release_view_lock();
    if (invalidate) invalidate_core_nonblocking();
}

void suite_translation_ready(void)
{
    if (!s_started || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    if (!s_view.worker_busy && s_view.state != SUITE_TRANSLATION_WORKING &&
        s_view.state != SUITE_TRANSLATION_IDLE) {
        clear_text_locked();
        s_view.expires_at_ms = 0;
        publish_locked(SUITE_TRANSLATION_IDLE, "请选择翻译");
    }
    release_view_lock();
}

bool suite_translation_reset_mock_session(void)
{
    if (!s_started || !s_mutex || !s_core_mutex) return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    bool allowed = !s_view.worker_busy &&
                   s_view.state == SUITE_TRANSLATION_BUSY;
    release_view_lock();
    if (!allowed) return false;

    if (xSemaphoreTake(s_core_mutex, 0) != pdTRUE) return false;
    tr_core_init(&s_core);
    xSemaphoreGive(s_core_mutex);

    unsigned epoch = atomic_fetch_add(&s_identity_epoch, 1U) + 1U;
    unsigned session = atomic_fetch_add(&s_session, 1U) + 1U;
    atomic_store(&s_reconcile_required, true);
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_store(&s_enter_session, session);
        atomic_store(&s_enter_pending, true);
        atomic_store(&s_reconcile_required, true);
        return true;
    }
    atomic_store(&s_enter_pending, false);
    atomic_store(&s_exit_invalidated, false);
    atomic_store(&s_cancel_requested, false);
    s_view.session = session;
    s_view.identity_epoch = epoch;
    s_view.core_generation = 0;
    clear_text_locked();
    s_view.expires_at_ms = 0;
    publish_locked(SUITE_TRANSLATION_IDLE, "本机记录已清除，可继续");
    release_view_lock();
    return true;
}

void suite_translation_poll(void)
{
    if (!s_started || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    bool invalidate = reconcile_pending_locked();
    expire_locked(now_ms());
    release_view_lock();
    if (invalidate) invalidate_core_nonblocking();
}

bool suite_translation_set_provider(const suite_translation_provider_t *provider)
{
    if (!s_started || !s_mutex) return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    bool accepted = !s_view.worker_busy;
    if (accepted) s_provider = provider ? provider : &suite_translation_mock_provider;
    release_view_lock();
    return accepted;
}

const suite_translation_view_t *suite_translation_view_acquire(void)
{
    if (!s_started || !s_mutex) return &s_unavailable;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return NULL;
    (void)reconcile_pending_locked();
    return &s_view;
}

void suite_translation_view_release(void)
{
    if (s_mutex) release_view_lock();
}
