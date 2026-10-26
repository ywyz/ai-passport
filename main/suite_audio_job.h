#pragma once
#include "esp_err.h"
#include "suite_task_voice.h"
#include "suite_tasks_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared inactive-slot audio scratch ownership.
 *
 * The authoritative Tasks cache object owns both Flash slots. This module adds
 * the outer S lease over that object and over the cache generation; it never
 * creates a second cache. Lock order is S then A (audio) or S then N (network);
 * A and N are never held together and S/A is never acquired while N is held.
 *
 * Scratch uses the qualified inactive slot only. A lease captures the active
 * generation, so Tasks refresh cannot commit a different generation underneath
 * a capture/upload owner. Cleanup erases data before the marker sector; a
 * failed erase quarantines that slot for the rest of the boot and refuses reuse.
 */

#define SUITE_AUDIO_WAV_HEADER 44U
#define SUITE_AUDIO_WAV_MAX 320044U
#define SUITE_AUDIO_PCM_MAX (SUITE_AUDIO_WAV_MAX - SUITE_AUDIO_WAV_HEADER)
#define SUITE_AUDIO_CHUNK_MAX 512U
#define SUITE_AUDIO_SPAN_MAX TASK_VOICE_ERASE
/* T1 translation capture contract: 10 s maximum and at least 0.3 s PCM at
   16 kHz mono PCM16 (32,000 bytes/s). The caller configures the BSP rate. */
#define SUITE_AUDIO_TRANSLATION_SPAN 0x50000U
#define SUITE_AUDIO_TRANSLATION_PCM_MIN 9600U
#define SUITE_AUDIO_TRANSLATION_PCM_MAX SUITE_AUDIO_PCM_MAX

typedef enum {
    SUITE_AUDIO_OWNER_NONE = 0,
    SUITE_AUDIO_OWNER_TASKS_REFRESH = 1,
    SUITE_AUDIO_OWNER_TASKS_VOICE = 2,
    SUITE_AUDIO_OWNER_TRANSLATION = 3
} suite_audio_owner_t;

typedef struct suite_audio_lease suite_audio_lease_t;

/* Create S once at boot. Idempotent; suite_tasks_start also calls it. */
esp_err_t suite_audio_job_init(void);

/* Bind the authoritative cache and its partition I/O after task_cache_open().
   Rebinding is allowed only while no lease is held. */
esp_err_t suite_audio_job_bind(task_cache_t *cache, task_io_t io);

/* Bounded lease acquisition. cancelled() is polled while waiting; ctx is
   caller-owned. timeout_ms=0 performs one non-blocking attempt. */
esp_err_t suite_audio_job_acquire_ex(suite_audio_owner_t owner, uint32_t timeout_ms,
                                     bool (*cancelled)(void *), void *ctx,
                                     suite_audio_lease_t **out);
esp_err_t suite_audio_job_acquire(suite_audio_owner_t owner, uint32_t timeout_ms,
                                  suite_audio_lease_t **out);

/* Lease observation. Valid until release. */
suite_audio_owner_t suite_audio_job_owner_of(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_generation(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_base(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_span(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_pcm_max(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_pcm_bytes(const suite_audio_lease_t *lease);
uint32_t suite_audio_job_wav_bytes(const suite_audio_lease_t *lease);
bool suite_audio_job_is_cancelled(const suite_audio_lease_t *lease);
bool suite_audio_job_stage_started(const suite_audio_lease_t *lease);

/* Erase the qualified inactive slot and write its legacy-compatible scratch
   marker. span is sector aligned, between TASK_DATA_START+4096 and
   SUITE_AUDIO_SPAN_MAX. Returns the PCM data base (slot base + TASK_DATA_START)
   through the lease accessor. */
esp_err_t suite_audio_job_stage_begin(suite_audio_lease_t *lease, uint32_t span);

/* Append PCM16LE in bounded chunks. The canonical WAV header is written only by
   finalize, so a partial clip is never reported as finalized. */
esp_err_t suite_audio_job_append(suite_audio_lease_t *lease, const void *pcm, size_t bytes);
esp_err_t suite_audio_job_finalize(suite_audio_lease_t *lease);

/* Bounded read-back of the finalized WAV. */
esp_err_t suite_audio_job_read(suite_audio_lease_t *lease, uint32_t offset,
                               void *out, size_t bytes);
/* Read the finalized WAV in bounded chunks and hash exactly its on-Flash bytes. */
esp_err_t suite_audio_job_sha256(suite_audio_lease_t *lease, uint8_t digest[32]);

/* Cancellation is advisory; the owner still calls release so cleanup and S
   release happen on the worker that owns them. abort() cancels then releases. */
void suite_audio_job_cancel(suite_audio_lease_t *lease);
esp_err_t suite_audio_job_abort(suite_audio_lease_t *lease);

/* Clear any staged scratch, release S, and return cleanup status. Always gives
   S, including on cleanup failure; the failed slot is quarantined. */
esp_err_t suite_audio_job_release(suite_audio_lease_t *lease);

/* Startup/retry recovery. Only the inactive slot is considered when a valid
   cache exists; both slots are considered only when no generation is valid.
   Recognizes the legacy scratch marker, so an older firmware's interrupted
   recording does not disable the updated firmware. */
esp_err_t suite_audio_job_recover(void);

/* Read-only diagnostics; no audio, token or host identity is exposed. */
bool suite_audio_job_ready(void);
bool suite_audio_job_held(void);
suite_audio_owner_t suite_audio_job_owner(void);
bool suite_audio_job_slot_quarantined(unsigned slot);

#ifdef SUITE_AUDIO_JOB_TEST
/* Host-test harness reset. Not part of the firmware ABI. */
void suite_audio_job_test_reset(void);
#endif
