/* Real shared scratch lease against the real Tasks cache object. */
#define _POSIX_C_SOURCE 200809L
#define SUITE_AUDIO_JOB_TEST 1
#include "suite_audio_job.h"
#include "suite_tasks_core.h"
#include "mbedtls/sha256.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t memory[2 * TASK_SLOT_SIZE];
static unsigned erase_fail_slot = 99;
static unsigned marker_read_fail_slot = 99;
static unsigned erase_log[64];
static unsigned erase_count;

static bool io_read(void *ctx, uint32_t at, void *out, size_t bytes)
{
    (void)ctx;
    assert((size_t)at + bytes <= sizeof(memory));
    if (!(at % TASK_SLOT_SIZE) && bytes == sizeof(uint32_t) &&
        at / TASK_SLOT_SIZE == marker_read_fail_slot) return false;
    memcpy(out, memory + at, bytes);
    return true;
}
static bool io_write(void *ctx, uint32_t at, const void *in, size_t bytes)
{
    (void)ctx;
    assert((size_t)at + bytes <= sizeof(memory));
    memcpy(memory + at, in, bytes);
    return true;
}
static bool io_erase(void *ctx, uint32_t at, size_t bytes)
{
    (void)ctx;
    assert(!(at % 4096) && !(bytes % 4096));
    if (erase_count < sizeof(erase_log) / sizeof(erase_log[0]))
        erase_log[erase_count] = at / TASK_SLOT_SIZE;
    erase_count++;
    unsigned slot = at / TASK_SLOT_SIZE;
    if (slot == erase_fail_slot) return false;
    memset(memory + at, 0xff, bytes);
    return true;
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
void test_log(const char *tag, const char *fmt, ...)
{
    (void)tag; (void)fmt;
}
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)

struct test_sem { pthread_mutex_t mutex; };
static struct test_sem semaphore = {.mutex = PTHREAD_MUTEX_INITIALIZER};
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &semaphore; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (!ticks) return pthread_mutex_trylock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += ticks / 1000;
    limit.tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (limit.tv_nsec >= 1000000000L) { limit.tv_sec++; limit.tv_nsec -= 1000000000L; }
    int result;
    do { result = pthread_mutex_timedlock(&sem->mutex, &limit); }
    while (result == EINTR);
    return result == 0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) { return pthread_mutex_unlock(&sem->mutex) == 0 ? pdTRUE : pdFALSE; }

/* Minimal SHA-256 for host verification only. */
static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(mbedtls_sha256_context *c, const unsigned char *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->state[0],b=c->state[1],cc=c->state[2],d=c->state[3],e=c->state[4],f=c->state[5],g=c->state[6],h=c->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}
static void sha_init(mbedtls_sha256_context *c)
{
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->state, iv, sizeof(iv)); c->total = 0; c->used = 0;
}
static void sha_update(mbedtls_sha256_context *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->total += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buffer + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha_block(c, c->buffer); c->used = 0; }
    }
}
static void sha_final(mbedtls_sha256_context *c, unsigned char out[32])
{
    uint64_t bits = c->total * 8;
    unsigned char pad = 0x80;
    sha_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->used != 56) sha_update(c, &zero, 1);
    unsigned char length[8];
    for (unsigned i = 0; i < 8; i++) length[i] = (unsigned char)(bits >> (56 - 8*i));
    sha_update(c, length, 8);
    for (unsigned i = 0; i < 8; i++) {
        out[4*i] = (unsigned char)(c->state[i] >> 24);
        out[4*i+1] = (unsigned char)(c->state[i] >> 16);
        out[4*i+2] = (unsigned char)(c->state[i] >> 8);
        out[4*i+3] = (unsigned char)c->state[i];
    }
}
void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    (void)is224; sha_init((mbedtls_sha256_context *)ctx); return 0;
}
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *in, size_t n)
{
    sha_update((mbedtls_sha256_context *)ctx, in, n); return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char out[32])
{
    sha_final((mbedtls_sha256_context *)ctx, out); return 0;
}
int mbedtls_sha256(const unsigned char *in, size_t n, unsigned char out[32], int is224)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_starts(&ctx, is224);
    mbedtls_sha256_update(&ctx, in, n);
    mbedtls_sha256_finish(&ctx, out);
    return 0;
}

static const char *empty_page =
    "{\"schema\":1,\"snapshot_revision\":\"0123456789abcdef0123456789abcdef\","
    "\"selection_revision\":1,\"downloaded_at\":1792368000,\"state\":\"EMPTY\","
    "\"read_only\":true,\"total\":0,\"offset\":0,\"next_offset\":null,\"items\":[]}";

static void commit_empty(task_cache_t *cache)
{
    assert(task_cache_begin(cache));
    uint32_t at = (uint32_t)cache->staging * TASK_SLOT_SIZE + TASK_SCRATCH;
    memcpy(memory + at, empty_page, strlen(empty_page));
    task_page_t page;
    task_record_t record;
    assert(task_cache_page(cache, strlen(empty_page), &page, &record));
    assert(task_cache_commit(cache, 1792368010));
}

static void reset_all(task_cache_t *cache)
{
    suite_audio_job_test_reset();
    memset(memory, 0xff, sizeof(memory));
    erase_fail_slot = 99;
    marker_read_fail_slot = 99;
    erase_count = 0;
    task_io_t io = {.read=io_read, .write=io_write, .erase=io_erase};
    memset(cache, 0, sizeof(*cache));
    (void)task_cache_open(cache, io);
    assert(!cache->valid);
    commit_empty(cache);          /* slot 0, generation 1 */
    commit_empty(cache);          /* slot 1, generation 2 */
    assert(cache->valid && cache->slot == 1 && cache->active.generation == 2);
    assert(suite_audio_job_init() == ESP_OK);
    assert(suite_audio_job_bind(cache, io) == ESP_OK);
}

static void check_hash(const suite_audio_lease_t *lease, uint32_t bytes)
{
    unsigned char got[32], expected[32];
    mbedtls_sha256_context ctx;
    sha_init(&ctx);
    unsigned char header[44];
    assert(io_read(NULL, suite_audio_job_base(lease), header, sizeof(header)));
    sha_update(&ctx, header, sizeof(header));
    for (uint32_t offset = 0; offset < bytes;) {
        unsigned char chunk[SUITE_AUDIO_CHUNK_MAX];
        size_t n = bytes - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        assert(io_read(NULL, suite_audio_job_base(lease) + 44 + offset, chunk, n));
        sha_update(&ctx, chunk, n);
        offset += (uint32_t)n;
    }
    sha_final(&ctx, expected);
    assert(suite_audio_job_sha256((suite_audio_lease_t *)lease, got) == ESP_OK);
    assert(!memcmp(got, expected, sizeof(got)));
}

static bool cancelled_cb(void *ctx)
{
    return *(const bool *)ctx;
}

int main(void)
{
    task_cache_t cache;
    suite_audio_lease_t *lease = NULL;

    /* Real lease, bounded acquisition, canonical WAV and bounded read/hash. */
    reset_all(&cache);
    uint8_t active_before[TASK_SLOT_SIZE];
    memcpy(active_before, memory + TASK_SLOT_SIZE, sizeof(active_before));
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_OK);
    assert(suite_audio_job_held() && suite_audio_job_owner() == SUITE_AUDIO_OWNER_TRANSLATION);
    assert(suite_audio_job_generation(lease) == 2 && suite_audio_job_base(lease) == 0);
    suite_audio_lease_t *blocked = NULL;
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TASKS_REFRESH, 20, &blocked) == ESP_ERR_TIMEOUT);
    assert(!blocked && suite_audio_job_held());
    assert(suite_audio_job_stage_begin(lease, TASK_DATA_START) == ESP_ERR_INVALID_ARG);
    assert(suite_audio_job_stage_begin(lease, 0x50000) == ESP_OK);
    assert(suite_audio_job_stage_started(lease) && suite_audio_job_base(lease) == TASK_DATA_START);
    unsigned char pcm[SUITE_AUDIO_CHUNK_MAX];
    for (unsigned i = 0; i < sizeof(pcm); i++) pcm[i] = (unsigned char)(i * 7U + 3U);
    for (unsigned i = 0; i < 20; i++) assert(suite_audio_job_append(lease, pcm, sizeof(pcm)) == ESP_OK);
    unsigned char odd[3] = {0};
    assert(suite_audio_job_append(lease, odd, 3) == ESP_ERR_INVALID_ARG);
    assert(suite_audio_job_append(lease, pcm, SUITE_AUDIO_CHUNK_MAX + 2) == ESP_ERR_INVALID_ARG);
    assert(suite_audio_job_finalize(lease) == ESP_OK);
    assert(suite_audio_job_pcm_bytes(lease) == 20 * SUITE_AUDIO_CHUNK_MAX);
    assert(suite_audio_job_wav_bytes(lease) == 20 * SUITE_AUDIO_CHUNK_MAX + 44);
    unsigned char header[44];
    assert(suite_audio_job_read(lease, 0, header, sizeof(header)) == ESP_OK);
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
    assert(header[22] == 1 && header[34] == 16 && header[24] == 0x80 && header[25] == 0x3e);
    assert(suite_audio_job_append(lease, pcm, 2) == ESP_ERR_INVALID_STATE);
    unsigned char readback[SUITE_AUDIO_CHUNK_MAX];
    assert(suite_audio_job_read(lease, 0, readback, sizeof(readback)) == ESP_OK);
    assert(suite_audio_job_read(lease, suite_audio_job_wav_bytes(lease), readback, 1) == ESP_ERR_INVALID_ARG);
    check_hash(lease, 20 * SUITE_AUDIO_CHUNK_MAX);
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TASKS_VOICE, 0, &blocked) == ESP_ERR_TIMEOUT);

    /* Cleanup erases data before the marker and leaves the active generation. */
    erase_count = 0;
    assert(suite_audio_job_release(lease) == ESP_OK);
    assert(!suite_audio_job_held());
    assert(erase_count == 2 && erase_log[0] == 0 && erase_log[1] == 0);
    assert(*(const uint32_t *)memory != 0x31445541U);
    assert(!memcmp(active_before, memory + TASK_SLOT_SIZE, sizeof(active_before)));
    assert(cache.active.generation == 2 && cache.slot == 1);

    /* Quarantine on failed erase, then idempotent restart recovery. */
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_OK);
    assert(suite_audio_job_stage_begin(lease, 0x50000) == ESP_OK);
    assert(suite_audio_job_append(lease, pcm, sizeof(pcm)) == ESP_OK);
    erase_fail_slot = 0;
    assert(suite_audio_job_release(lease) == ESP_FAIL);
    assert(suite_audio_job_slot_quarantined(0) && !suite_audio_job_held());
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_ERR_INVALID_STATE);
    erase_fail_slot = 99;
    assert(suite_audio_job_recover() == ESP_OK && !suite_audio_job_slot_quarantined(0));
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_OK);
    assert(suite_audio_job_abort(lease) == ESP_OK);

    /* Cancellation while waiting is bounded and does not take S. */
    bool stop = true;
    assert(suite_audio_job_acquire_ex(SUITE_AUDIO_OWNER_TRANSLATION, 1000,
        cancelled_cb, &stop, &blocked) == ESP_ERR_INVALID_STATE);
    assert(!blocked && !suite_audio_job_held());

    uint32_t magic = 0x31445541U;
    /* An unreadable marker is not clean: cleanup fails without erasing and
       quarantines the slot; recovery with the same fault preserves it. */
    reset_all(&cache);
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_OK);
    assert(suite_audio_job_stage_begin(lease, 0x50000) == ESP_OK);
    assert(suite_audio_job_append(lease, pcm, sizeof(pcm)) == ESP_OK);
    marker_read_fail_slot = 0;
    erase_count = 0;
    assert(suite_audio_job_release(lease) == ESP_FAIL);
    assert(erase_count == 0 && suite_audio_job_slot_quarantined(0));
    assert(*(const uint32_t *)memory == 0x31445541U);
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 0, &lease) == ESP_ERR_INVALID_STATE);
    marker_read_fail_slot = 99;
    assert(suite_audio_job_recover() == ESP_OK);
    assert(!suite_audio_job_slot_quarantined(0));
    assert(*(const uint32_t *)memory != 0x31445541U);

    reset_all(&cache);
    memcpy(memory + 0, &magic, sizeof(magic));
    marker_read_fail_slot = 0;
    erase_count = 0;
    assert(suite_audio_job_recover() == ESP_FAIL);
    assert(erase_count == 0 && suite_audio_job_slot_quarantined(0));
    assert(*(const uint32_t *)memory == magic);
    marker_read_fail_slot = 99;
    assert(suite_audio_job_recover() == ESP_OK);
    assert(!suite_audio_job_slot_quarantined(0));

    /* Legacy marker compatibility: a marker in the inactive slot is erased, a
     * marker in the active slot is never touched by startup recovery. */
    memcpy(memory + 0, &magic, sizeof(magic));
    memcpy(memory + TASK_SLOT_SIZE, &magic, sizeof(magic));
    assert(suite_audio_job_recover() == ESP_OK);
    assert(*(const uint32_t *)memory != magic);
    assert(*(const uint32_t *)(memory + TASK_SLOT_SIZE) == magic);

    /* Without a valid generation both slots are inactive and eligible. */
    reset_all(&cache);
    memcpy(memory + 0, &magic, sizeof(magic));
    memcpy(memory + TASK_SLOT_SIZE, &magic, sizeof(magic));
    memset(&cache, 0, sizeof(cache));
    task_io_t io = {.read=io_read, .write=io_write, .erase=io_erase};
    assert(!task_cache_open(&cache, io));
    assert(suite_audio_job_bind(&cache, io) == ESP_OK);
    assert(suite_audio_job_recover() == ESP_OK);
    assert(*(const uint32_t *)memory != magic);
    assert(*(const uint32_t *)(memory + TASK_SLOT_SIZE) != magic);

    puts("shared scratch lease / WAV / hash / cleanup order / quarantine / legacy marker: PASS");
    return 0;
}
