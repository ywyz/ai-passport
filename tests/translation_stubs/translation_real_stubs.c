#define _DEFAULT_SOURCE
#include "translation_real_stubs.h"

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/platform_util.h"
#include "suite_audio_job.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_translation.h"
#include "suite_translation_usb.h"

#include <stdatomic.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

translation_real_stub_t tr_stub;

struct suite_audio_lease {
    suite_audio_owner_t owner;
    uint32_t generation, base, span, pcm, pcm_max;
    bool active, staged, finalized, cancelled;
};

static struct suite_audio_lease s_lease;

static void order(int value)
{
    if (tr_stub.order_count < sizeof(tr_stub.order) / sizeof(tr_stub.order[0]))
        tr_stub.order[tr_stub.order_count++] = value;
}

void translation_real_stub_reset(void)
{
    memset(&tr_stub, 0, sizeof(tr_stub));
    tr_stub.config_status = SUITE_TRANSLATION_CONFIG_OK;
    tr_stub.online = true;
    tr_stub.time_trusted = true;
    tr_stub.ready_payload = true;
    tr_stub.pcm_max = 64000U;
    tr_stub.read_delay_us = 2000U;
    tr_stub.heap_free_bytes = 65536U;
    tr_stub.heap_largest_bytes = 32768U;
    memset(&s_lease, 0, sizeof(s_lease));
}

suite_translation_config_status_t suite_translation_config_get(
    suite_translation_config_t *out)
{
    if (!out) return SUITE_TRANSLATION_CONFIG_INVALID;
    memset(out, 0, sizeof(*out));
    if (tr_stub.config_status != SUITE_TRANSLATION_CONFIG_OK)
        return tr_stub.config_status;
    strcpy(out->origin, "https://translation.test");
    strcpy(out->token, "translation_capability_0123456789abcdef");
    out->schema = SUITE_TRANSLATION_CONFIG_SCHEMA;
    return SUITE_TRANSLATION_CONFIG_OK;
}

suite_conn_state_t suite_connection_state(void)
{
    return tr_stub.online ? SUITE_CONN_ONLINE : SUITE_CONN_OFF;
}
int64_t suite_connection_now(void) { return 1792368000; }
static suite_time_tracker_t s_tracker = {
    .state = SUITE_TIME_SYNCED, .last_sync_epoch = 1792368000 };
const suite_time_tracker_t *suite_connection_time(void) { return &s_tracker; }
bool suite_time_is_trusted(const suite_time_tracker_t *tracker, int64_t now)
{
    (void)tracker; (void)now; return tr_stub.time_trusted;
}

bool suite_translation_usb_open_window(void)
{
    if (!tr_stub.usb_open_result) return false;
    tr_stub.usb_window_open = true;
    return true;
}
void suite_translation_usb_cancel(void) { tr_stub.usb_window_open = false; }
bool suite_translation_usb_window_open(void) { return tr_stub.usb_window_open; }
void suite_translation_usb_set_busy_check(bool (*busy)(void)) { (void)busy; }

esp_err_t bsp_audio_init(void)
{
    order(TR_ORDER_AUDIO_INIT);
    return tr_stub.audio_init_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch)
{
    (void)hz; (void)bits; (void)ch;
    return tr_stub.audio_format_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_audio_read(void *pcm, size_t bytes)
{
    tr_stub.audio_reads++;
    if (tr_stub.audio_read_fail &&
        (!tr_stub.audio_read_fail_at ||
         tr_stub.audio_reads == tr_stub.audio_read_fail_at))
        return ESP_FAIL;
    if (tr_stub.read_delay_us) usleep(tr_stub.read_delay_us);
    memset(pcm, 0x5a, bytes);
    return ESP_OK;
}
esp_err_t bsp_audio_release(void)
{
    order(TR_ORDER_AUDIO_RELEASE);
    tr_stub.audio_release_calls++;
    return tr_stub.audio_release_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t suite_audio_job_acquire_ex(suite_audio_owner_t owner, uint32_t timeout_ms,
                                     bool (*cancelled)(void *), void *ctx,
                                     suite_audio_lease_t **out)
{
    (void)timeout_ms;
    order(TR_ORDER_ACQUIRE);
    if (out) *out = NULL;
    tr_stub.acquire_calls++;
    if (cancelled && cancelled(ctx)) return ESP_ERR_INVALID_STATE;
    if (tr_stub.acquire_fail) return ESP_FAIL;
    memset(&s_lease, 0, sizeof(s_lease));
    s_lease.owner = owner;
    s_lease.generation = 7;
    s_lease.pcm_max = tr_stub.pcm_max;
    s_lease.active = true;
    if (out) *out = &s_lease;
    return ESP_OK;
}
esp_err_t suite_audio_job_stage_begin(suite_audio_lease_t *lease, uint32_t span)
{
    order(TR_ORDER_STAGE);
    tr_stub.stage_calls++;
    if (!lease || tr_stub.stage_fail) return ESP_FAIL;
    lease->base = 0x1000;
    lease->span = span;
    lease->pcm = 0;
    lease->staged = true;
    return ESP_OK;
}
esp_err_t suite_audio_job_append(suite_audio_lease_t *lease, const void *pcm,
                                 size_t bytes)
{
    (void)pcm;
    if (!lease || tr_stub.append_fail) return ESP_FAIL;
    if (lease->pcm + bytes > lease->pcm_max) return ESP_FAIL;
    lease->pcm += (uint32_t)bytes;
    tr_stub.pcm_bytes = lease->pcm;
    return ESP_OK;
}
esp_err_t suite_audio_job_finalize(suite_audio_lease_t *lease)
{
    if (!lease || tr_stub.finalize_fail) return ESP_FAIL;
    lease->finalized = true;
    return ESP_OK;
}
esp_err_t suite_audio_job_sha256(suite_audio_lease_t *lease, uint8_t digest[32])
{
    (void)lease;
    if (tr_stub.sha_fail) return ESP_FAIL;
    for (unsigned i = 0; i < 32; ++i) digest[i] = (uint8_t)(i + 1);
    /* Test the window after hashing and before the admit POST. */
    if (tr_stub.cancel_on_sha) suite_translation_cancel();
    if (tr_stub.exit_on_sha) suite_translation_session_exit();
    return ESP_OK;
}
uint32_t suite_audio_job_wav_bytes(const suite_audio_lease_t *lease)
{
    return lease && lease->finalized ? lease->pcm + 44U : 0U;
}
uint32_t suite_audio_job_pcm_max(const suite_audio_lease_t *lease)
{
    return lease ? lease->pcm_max : 0U;
}
uint32_t suite_audio_job_pcm_bytes(const suite_audio_lease_t *lease)
{
    return lease ? lease->pcm : 0U;
}
bool suite_audio_job_is_cancelled(const suite_audio_lease_t *lease)
{
    return lease && lease->cancelled;
}
esp_err_t suite_audio_job_read(suite_audio_lease_t *lease, uint32_t offset,
                               void *out, size_t bytes)
{
    if (!lease || !lease->finalized || !out) return ESP_FAIL;
    uint32_t total = lease->pcm + 44U;
    if (offset > total || bytes > total - offset) return ESP_FAIL;
    memset(out, 0x33, bytes);
    return ESP_OK;
}
esp_err_t suite_audio_job_release(suite_audio_lease_t *lease)
{
    order(TR_ORDER_CLEANUP);
    tr_stub.cleanup_calls++;
    if (lease) memset(lease, 0, sizeof(*lease));
    return tr_stub.cleanup_fail ? ESP_FAIL : ESP_OK;
}
bool suite_audio_job_held(void) { return s_lease.active; }

suite_translation_http_result_t suite_translation_http_init(
    suite_translation_http_t *http, const char *origin, const char *token,
    uint8_t *json, size_t json_cap)
{
    if (!http || !origin || !token || json_cap < SUITE_TR_HTTP_JSON_CAP)
        return SUITE_TR_HTTP_E_ARGUMENT;
    assert(json == NULL); /* The real worker uses per-call transport storage. */
    http->origin = origin;
    http->token = token;
    http->json = json;
    http->json_cap = json_cap;
    return SUITE_TR_HTTP_OK;
}

static void assert_no_deferred_storage(const suite_translation_http_t *http)
{
    assert(http != NULL);
    assert(http->json == NULL);
}

suite_translation_http_result_t suite_translation_http_ready(
    const suite_translation_http_t *http, const suite_translation_http_call_t *call,
    suite_translation_protocol_ready_t *ready, suite_translation_http_info_t *info)
{
    assert_no_deferred_storage(http);
    (void)http; (void)call;
    order(TR_ORDER_READY);
    tr_stub.http_ready_calls++;
    if (info) info->http_status = (int)tr_stub.ready_http_status;
    if (info && tr_stub.ready_tls_diag_valid &&
        tr_stub.ready_result == SUITE_TR_HTTP_E_TLS)
        info->tls_diag = tr_stub.ready_tls_diag;
    if (tr_stub.exit_during_ready) {
        suite_translation_session_exit();
        return SUITE_TR_HTTP_E_CANCELLED;
    }
    if (tr_stub.cancel_during_ready) {
        suite_translation_cancel();
        return SUITE_TR_HTTP_E_CANCELLED;
    }
    if (ready) { ready->ready = tr_stub.ready_payload; strcpy(ready->provider, "mimo"); }
    return tr_stub.ready_result;
}
suite_translation_http_result_t suite_translation_http_admit(
    const suite_translation_http_t *http, const suite_translation_http_call_t *call,
    const char *operation_id, suite_translation_protocol_direction_t direction,
    const char *audio_sha256, uint32_t audio_bytes,
    suite_translation_protocol_view_t *view, suite_translation_http_info_t *info)
{
    assert_no_deferred_storage(http);
    (void)http; (void)call;
    order(TR_ORDER_ADMIT);
    if (audio_sha256) {
        strncpy(tr_stub.admitted_sha, audio_sha256, 64);
        tr_stub.admitted_sha[64] = 0;
    }
    tr_stub.http_admit_calls++;
    if (tr_stub.cancel_during_admit) {
        suite_translation_cancel();
        return SUITE_TR_HTTP_E_CANCELLED;
    }
    if (info) info->ambiguous = tr_stub.admit_ambiguous;
    if (view) {
        strcpy(view->operation_id, operation_id);
        strcpy(view->job_id, tr_stub.admit_bad_identity
                                 ? "bad"
                                 : "0123456789abcdef0123456789abcdef");
        view->generation = 3;
        view->direction = direction;
        view->audio_bytes = audio_bytes;
        view->state = SUITE_TR_PROTOCOL_STATE_ADMITTED;
    }
    return tr_stub.admit_result;
}
suite_translation_http_result_t suite_translation_http_upload(
    const suite_translation_http_t *http, const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_http_audio_read_fn read, void *read_ctx,
    suite_translation_protocol_view_t *view, suite_translation_http_info_t *info)
{
    assert_no_deferred_storage(http);
    (void)http; (void)call; (void)expected; (void)view;
    order(TR_ORDER_UPLOAD);
    tr_stub.http_upload_calls++;
    if (tr_stub.cancel_during_upload) {
        suite_translation_cancel();
        return SUITE_TR_HTTP_E_CANCELLED;
    }
    if (info) info->ambiguous = tr_stub.upload_ambiguous;
    uint8_t buffer[512];
    for (uint32_t offset = 0; offset < expected->audio_bytes;) {
        size_t n = read(read_ctx, offset, buffer, sizeof(buffer));
        if (!n) return SUITE_TR_HTTP_E_IO;
        offset += (uint32_t)n;
        tr_stub.upload_reader_bytes += (unsigned)n;
    }
    return tr_stub.upload_result;
}
suite_translation_http_result_t suite_translation_http_status(
    const suite_translation_http_t *http, const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view, suite_translation_http_info_t *info)
{
    assert_no_deferred_storage(http);
    (void)http; (void)call; (void)expected;
    order(TR_ORDER_STATUS);
    tr_stub.http_status_calls++;
    if (tr_stub.cancel_during_status) {
        suite_translation_cancel();
        return SUITE_TR_HTTP_E_CANCELLED;
    }
    if (info) info->ambiguous = tr_stub.status_ambiguous;
    if (!view) return SUITE_TR_HTTP_E_ARGUMENT;
    unsigned index = tr_stub.status_index;
    if (index >= tr_stub.status_count) index = tr_stub.status_count ? tr_stub.status_count - 1 : 0;
    view->state = tr_stub.status_states[index];
    if (tr_stub.status_index + 1 < tr_stub.status_count) tr_stub.status_index++;
    view->has_source = tr_stub.status_source;
    view->has_target = tr_stub.status_target;
    view->source_length = 0;
    view->target_length = 0;
    if (view->has_source && view->source && view->source_cap > 5) {
        strcpy(view->source, "Hello");
        view->source_length = 5;
    }
    if (view->has_target && view->target && view->target_cap > 6) {
        strcpy(view->target, "你好");
        view->target_length = 6;
    }
    return tr_stub.status_result;
}
suite_translation_http_result_t suite_translation_http_cancel(
    const suite_translation_http_t *http, const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view, suite_translation_http_info_t *info)
{
    assert_no_deferred_storage(http);
    (void)http; (void)call; (void)expected; (void)view; (void)info;
    order(TR_ORDER_CANCEL);
    tr_stub.http_cancel_calls++;
    return tr_stub.cancel_result;
}

void mbedtls_platform_zeroize(void *data, size_t size) { memset(data, 0, size); }
size_t heap_caps_get_free_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_DMA) return 0;
    if (tr_stub.heap_free_calls++ == 0U)
        tr_stub.heap_free_first = tr_stub.heap_free_bytes;
    if ((tr_stub.cancel_on_heap_sample || tr_stub.exit_on_heap_sample) &&
        !tr_stub.heap_hook_fired) {
        tr_stub.heap_hook_fired = true;
        if (tr_stub.cancel_on_heap_sample)
            suite_translation_cancel();
        else
            suite_translation_session_exit();
    }
    return tr_stub.heap_free_bytes;
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    if (caps & MALLOC_CAP_DMA) return 0;
    if (tr_stub.heap_largest_calls++ == 0U)
        tr_stub.heap_largest_first = tr_stub.heap_largest_bytes;
    return tr_stub.heap_largest_bytes;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 2048U;
}
