#include "suite_audio_job.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

#include <string.h>

static const char *AUDIO_TAG = "suite_audio_job";

#define SPAN_ALIGN 4096U
#define ACQUIRE_SLICE_MS 50U

struct suite_audio_lease {
    suite_audio_owner_t owner;
    unsigned slot;
    uint32_t generation, base, span, pcm, pcm_max;
    bool active, staged, finalized, cancelled;
};

static SemaphoreHandle_t s_sem;
static task_cache_t *s_audio_cache;
static task_io_t s_audio_io;
static bool s_bound, s_held, s_quarantined[2];
static suite_audio_owner_t s_owner;
static struct suite_audio_lease s_lease;

static bool inactive_slot(unsigned *slot)
{
    if (!s_bound || !s_audio_cache) return false;
    *slot = s_audio_cache->valid ? (unsigned)(1 - s_audio_cache->slot) : 0U;
    return *slot < 2U;
}

static bool lease_matches_audio_cache(const struct suite_audio_lease *lease)
{
    if (!s_audio_cache) return false;
    uint32_t generation = s_audio_cache->valid ? s_audio_cache->active.generation : 0U;
    return lease->generation == generation;
}

esp_err_t suite_audio_job_init(void)
{
    if (s_sem) return ESP_OK;
    s_sem = xSemaphoreCreateMutex();
    return s_sem ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t suite_audio_job_bind(task_cache_t *cache, task_io_t io)
{
    if (!cache || !io.read || !io.write || !io.erase) return ESP_ERR_INVALID_ARG;
    esp_err_t err = suite_audio_job_init();
    if (err != ESP_OK) return err;
    if (s_held) return ESP_ERR_INVALID_STATE;
    s_audio_cache = cache;
    s_audio_io = io;
    s_bound = true;
    return ESP_OK;
}

esp_err_t suite_audio_job_acquire_ex(suite_audio_owner_t owner, uint32_t timeout_ms,
                                     bool (*cancelled)(void *), void *ctx,
                                     suite_audio_lease_t **out)
{
    if (out) *out = NULL;
    if (!out || owner < SUITE_AUDIO_OWNER_TASKS_REFRESH || owner > SUITE_AUDIO_OWNER_TRANSLATION)
        return ESP_ERR_INVALID_ARG;
    if (!s_sem) return ESP_ERR_INVALID_STATE;
    if (!s_bound || !s_audio_cache) return ESP_ERR_INVALID_STATE;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (cancelled && cancelled(ctx)) return ESP_ERR_INVALID_STATE;
        uint32_t slice = ACQUIRE_SLICE_MS;
        if (timeout_ms) {
            int64_t remain_ms = (deadline - esp_timer_get_time()) / 1000;
            if (remain_ms <= 0) return ESP_ERR_TIMEOUT;
            if ((uint64_t)remain_ms < slice) slice = (uint32_t)remain_ms;
        }
        if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeout_ms ? slice : 0)) == pdTRUE) break;
        if (!timeout_ms) return ESP_ERR_TIMEOUT;
    }

    unsigned slot = 0;
    if (!inactive_slot(&slot) || s_quarantined[slot]) {
        xSemaphoreGive(s_sem);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_lease, 0, sizeof(s_lease));
    s_lease.owner = owner;
    s_lease.slot = slot;
    s_lease.generation = s_audio_cache->valid ? s_audio_cache->active.generation : 0U;
    s_lease.pcm_max = owner == SUITE_AUDIO_OWNER_TASKS_VOICE ?
        TASK_VOICE_PCM_MAX : SUITE_AUDIO_PCM_MAX;
    s_lease.active = true;
    s_held = true;
    s_owner = owner;
    *out = &s_lease;
    return ESP_OK;
}

esp_err_t suite_audio_job_acquire(suite_audio_owner_t owner, uint32_t timeout_ms,
                                  suite_audio_lease_t **out)
{
    return suite_audio_job_acquire_ex(owner, timeout_ms, NULL, NULL, out);
}

suite_audio_owner_t suite_audio_job_owner_of(const suite_audio_lease_t *lease)
{
    return lease ? lease->owner : SUITE_AUDIO_OWNER_NONE;
}
uint32_t suite_audio_job_generation(const suite_audio_lease_t *lease)
{
    return lease ? lease->generation : 0U;
}
uint32_t suite_audio_job_base(const suite_audio_lease_t *lease)
{
    return lease ? lease->base : 0U;
}
uint32_t suite_audio_job_span(const suite_audio_lease_t *lease)
{
    return lease ? lease->span : 0U;
}
uint32_t suite_audio_job_pcm_max(const suite_audio_lease_t *lease)
{
    return lease ? lease->pcm_max : 0U;
}
uint32_t suite_audio_job_pcm_bytes(const suite_audio_lease_t *lease)
{
    return lease ? lease->pcm : 0U;
}
uint32_t suite_audio_job_wav_bytes(const suite_audio_lease_t *lease)
{
    return lease && lease->finalized ? lease->pcm + SUITE_AUDIO_WAV_HEADER : 0U;
}
bool suite_audio_job_is_cancelled(const suite_audio_lease_t *lease)
{
    return lease && lease->cancelled;
}
bool suite_audio_job_stage_started(const suite_audio_lease_t *lease)
{
    return lease && lease->staged;
}

esp_err_t suite_audio_job_stage_begin(suite_audio_lease_t *lease, uint32_t span)
{
    if (!lease || !lease->active || lease->staged || lease->finalized)
        return ESP_ERR_INVALID_STATE;
    if (!s_bound || !s_audio_cache || lease != &s_lease || !lease_matches_audio_cache(lease))
        return ESP_ERR_INVALID_STATE;
    if (s_audio_cache->valid && s_audio_cache->slot == (int)lease->slot)
        return ESP_ERR_INVALID_STATE;
    if (span < TASK_DATA_START + SPAN_ALIGN || span > SUITE_AUDIO_SPAN_MAX ||
        span % SPAN_ALIGN)
        return ESP_ERR_INVALID_ARG;
    if (span < TASK_DATA_START + SUITE_AUDIO_WAV_HEADER + lease->pcm_max)
        return ESP_ERR_INVALID_ARG;
    uint32_t base = 0;
    if (!task_voice_stage_begin_span(s_audio_cache, lease->slot, span, &base)) {
        s_quarantined[lease->slot] = true;
        return ESP_FAIL;
    }
    lease->base = base;
    lease->span = span;
    lease->pcm = 0;
    lease->staged = true;
    lease->finalized = false;
    return ESP_OK;
}

esp_err_t suite_audio_job_append(suite_audio_lease_t *lease, const void *pcm, size_t bytes)
{
    if (!lease || !lease->active || !lease->staged || lease->finalized)
        return ESP_ERR_INVALID_STATE;
    if (!pcm || !bytes || bytes > SUITE_AUDIO_CHUNK_MAX || (bytes & 1U))
        return ESP_ERR_INVALID_ARG;
    if (bytes > lease->pcm_max - lease->pcm) return ESP_ERR_INVALID_ARG;
    if (!s_audio_io.write(s_audio_io.ctx, lease->base + SUITE_AUDIO_WAV_HEADER + lease->pcm, pcm, bytes))
        return ESP_FAIL;
    lease->pcm += (uint32_t)bytes;
    return ESP_OK;
}

esp_err_t suite_audio_job_finalize(suite_audio_lease_t *lease)
{
    if (!lease || !lease->active || !lease->staged || lease->finalized)
        return ESP_ERR_INVALID_STATE;
    if (!lease->pcm) return ESP_ERR_INVALID_STATE;
    unsigned char header[SUITE_AUDIO_WAV_HEADER];
    task_voice_wav(header, lease->pcm);
    if (!s_audio_io.write(s_audio_io.ctx, lease->base, header, sizeof(header))) return ESP_FAIL;
    lease->finalized = true;
    return ESP_OK;
}

esp_err_t suite_audio_job_read(suite_audio_lease_t *lease, uint32_t offset,
                               void *out, size_t bytes)
{
    if (!lease || !lease->active || !lease->finalized || !out)
        return ESP_ERR_INVALID_ARG;
    uint32_t total = lease->pcm + SUITE_AUDIO_WAV_HEADER;
    if (offset > total || bytes > total - offset) return ESP_ERR_INVALID_ARG;
    if (!bytes) return ESP_OK;
    return s_audio_io.read(s_audio_io.ctx, lease->base + offset, out, bytes) ? ESP_OK : ESP_FAIL;
}

esp_err_t suite_audio_job_sha256(suite_audio_lease_t *lease, uint8_t digest[32])
{
    if (!lease || !lease->active || !lease->finalized || !digest)
        return ESP_ERR_INVALID_ARG;
    uint32_t total = lease->pcm + SUITE_AUDIO_WAV_HEADER;
    unsigned char chunk[SUITE_AUDIO_CHUNK_MAX];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t result = ESP_FAIL;
    if (mbedtls_sha256_starts(&ctx, 0) != 0) goto done;
    for (uint32_t offset = 0; offset < total;) {
        size_t n = total - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!s_audio_io.read(s_audio_io.ctx, lease->base + offset, chunk, n)) goto done;
        if (mbedtls_sha256_update(&ctx, chunk, n) != 0) goto done;
        offset += (uint32_t)n;
    }
    if (mbedtls_sha256_finish(&ctx, digest) != 0) goto done;
    result = ESP_OK;
done:
    mbedtls_sha256_free(&ctx);
    return result;
}

void suite_audio_job_cancel(suite_audio_lease_t *lease)
{
    if (lease && lease->active) lease->cancelled = true;
}

static esp_err_t release_locked(struct suite_audio_lease *lease)
{
    esp_err_t result = ESP_OK;
    if (lease->staged) {
        bool safe = s_audio_cache && !(s_audio_cache->valid && s_audio_cache->slot == (int)lease->slot) &&
                    lease_matches_audio_cache(lease);
        if (!safe) {
            s_quarantined[lease->slot] = true;
            result = ESP_ERR_INVALID_STATE;
        } else if (!task_voice_stage_clear_slot(s_audio_cache, lease->slot)) {
            s_quarantined[lease->slot] = true;
            result = ESP_FAIL;
        }
        if (result == ESP_OK) s_quarantined[lease->slot] = false;
    }
    memset(&s_lease, 0, sizeof(s_lease));
    s_held = false;
    s_owner = SUITE_AUDIO_OWNER_NONE;
    xSemaphoreGive(s_sem);
    return result;
}

esp_err_t suite_audio_job_release(suite_audio_lease_t *lease)
{
    if (!lease || lease != &s_lease || !lease->active || !s_held)
        return ESP_ERR_INVALID_STATE;
    lease->active = false;
    esp_err_t result = release_locked(lease);
    if (result != ESP_OK) ESP_LOGE(AUDIO_TAG, "Scratch cleanup failed; inactive slot quarantined");
    return result;
}

esp_err_t suite_audio_job_abort(suite_audio_lease_t *lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    suite_audio_job_cancel(lease);
    return suite_audio_job_release(lease);
}

esp_err_t suite_audio_job_recover(void)
{
    if (!s_sem) return ESP_ERR_INVALID_STATE;
    if (!s_bound || !s_audio_cache) return ESP_ERR_INVALID_STATE;
    if (s_held) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_OK;
    unsigned slots[2];
    unsigned count = 0;
    if (s_audio_cache->valid) slots[count++] = (unsigned)(1 - s_audio_cache->slot);
    else { slots[count++] = 0U; slots[count++] = 1U; }
    for (unsigned i = 0; i < count; i++) {
        unsigned slot = slots[i];
        if (s_audio_cache->valid && s_audio_cache->slot == (int)slot) continue;
        int marker = task_voice_stage_marker(s_audio_cache, slot);
        if (marker < 0) {
            /* Unreadable marker: preserve it, keep the slot unusable and never
               report recovery as successful. */
            s_quarantined[slot] = true;
            result = ESP_FAIL;
            continue;
        }
        if (!marker) {
            s_quarantined[slot] = false;
            continue;
        }
        if (!task_voice_stage_clear_slot(s_audio_cache, slot)) {
            s_quarantined[slot] = true;
            result = ESP_FAIL;
        } else s_quarantined[slot] = false;
    }
    xSemaphoreGive(s_sem);
    return result;
}

bool suite_audio_job_ready(void) { return s_sem && s_bound && s_audio_cache; }
bool suite_audio_job_held(void) { return s_held; }
suite_audio_owner_t suite_audio_job_owner(void) { return s_owner; }
bool suite_audio_job_slot_quarantined(unsigned slot)
{
    return slot < 2U && s_quarantined[slot];
}

#ifdef SUITE_AUDIO_JOB_TEST
void suite_audio_job_test_reset(void)
{
    if (s_sem && s_held) {
        xSemaphoreGive(s_sem);
    }
    if (!s_sem) {
        s_sem = xSemaphoreCreateMutex();
    }
    memset(&s_lease, 0, sizeof(s_lease));
    s_audio_cache = NULL;
    memset(&s_audio_io, 0, sizeof(s_audio_io));
    s_bound = false;
    s_held = false;
    s_owner = SUITE_AUDIO_OWNER_NONE;
    s_quarantined[0] = s_quarantined[1] = false;
}
#endif
