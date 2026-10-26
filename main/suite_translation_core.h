#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure T1 translation bookkeeping for the provider-independent subset.
 *
 * This module has no ESP-IDF, LVGL, network, storage or clock dependency.
 * Callers own source/translation text; only identifiers, hashes, byte counts,
 * state and timestamps live in the bounded job table. No audio is retained.
 * The caller serializes access; no function here creates a task or lock.
 */

#define TR_TEXT_MAX_BYTES 2048U
#define TR_TEXT_MAX_SCALARS 512U
#define TR_WAV_MAX_BYTES 320044U
#define TR_WAV_MIN_DATA_BYTES 9600U
#define TR_ID_SIZE 33U
#define TR_SHA256_SIZE 65U
#define TR_TOKEN_MIN 32U
#define TR_TOKEN_MAX 128U
#define TR_METADATA_SLOTS 8U
#define TR_EVENT_LOG 8U

typedef enum {
    TR_LANG_EN = 1,
    TR_LANG_ZH_CN = 2
} tr_language_t;

typedef enum {
    TR_DIRECTION_EN_ZH = 1,
    TR_DIRECTION_ZH_EN = 2
} tr_direction_t;

typedef enum {
    TR_SCOPE_TRANSLATION = 1,
    TR_SCOPE_TASKS_READER = 2,
    TR_SCOPE_TASKS_WRITER = 3,
    TR_SCOPE_TASKS_ADMIN = 4
} tr_scope_t;

typedef enum {
    TR_OK = 0,
    TR_E_ARGUMENT,
    TR_E_SCOPE,
    TR_E_TOKEN,
    TR_E_DIRECTION,
    TR_E_ID,
    TR_E_SHA256,
    TR_E_GENERATION,
    TR_E_TEXT,
    TR_E_TEXT_BYTES,
    TR_E_TEXT_SCALARS,
    TR_E_AUDIO,
    TR_E_AUDIO_BYTES,
    TR_E_AUDIO_FORMAT,
    TR_E_AUDIO_CONFLICT,
    TR_E_STATE,
    TR_E_CONFLICT,
    TR_E_JOB,
    TR_E_NOT_FOUND,
    TR_E_BUSY,
    TR_E_CANCELLED,
    TR_E_LATE
} tr_error_t;

typedef enum {
    TR_STATE_EMPTY = 0,
    TR_STATE_ADMITTED = 1,
    TR_STATE_UPLOADED = 2,
    TR_STATE_RUNNING = 3,
    TR_STATE_DONE = 4,
    TR_STATE_FAILED = 5,
    TR_STATE_CANCELLED = 6,
    TR_STATE_UNKNOWN = 7
} tr_state_t;

typedef enum {
    TR_EVENT_NONE = 0,
    TR_EVENT_ADMITTED = 1,
    TR_EVENT_REPLAYED = 2,
    TR_EVENT_UPLOADED = 3,
    TR_EVENT_RUNNING = 4,
    TR_EVENT_DONE = 5,
    TR_EVENT_FAILED = 6,
    TR_EVENT_UNKNOWN = 7,
    TR_EVENT_CANCELLED = 8,
    TR_EVENT_REJECTED = 9
} tr_event_t;

typedef struct {
    tr_scope_t scope;
    char token[TR_TOKEN_MAX + 1U];
    uint8_t token_length;
} tr_capability_t;

typedef struct {
    char operation_id[TR_ID_SIZE];
    char job_id[TR_ID_SIZE];
    char audio_sha256[TR_SHA256_SIZE];
    uint32_t audio_bytes;
    uint32_t source_bytes;
    uint32_t target_bytes;
    uint32_t generation;
    int64_t created_at;
    int64_t updated_at;
    uint8_t direction;
    uint8_t state;
    uint8_t has_source;
    uint8_t has_target;
} tr_job_t;

typedef struct {
    const char *operation_id;
    tr_direction_t direction;
    const char *audio_sha256;
    uint32_t audio_bytes;
} tr_admission_t;

typedef struct {
    tr_job_t jobs[TR_METADATA_SLOTS];
    uint32_t count;
    uint32_t generation;
    uint8_t events[TR_EVENT_LOG];
    uint8_t event_head;
    uint8_t event_count;
} tr_core_t;

typedef struct {
    uint32_t bytes;
    uint32_t data_bytes;
    uint32_t seconds;
} tr_wav_info_t;

/* Only TR_SCOPE_TRANSLATION is accepted. Task scopes are rejected explicitly. */
tr_error_t tr_capability_init(tr_capability_t *capability, tr_scope_t scope,
                              const char *token, size_t token_length);

/*
 * Validate one UTF-8 text field without copying or truncating it.
 * Exact limits are 2048 bytes and 512 Unicode scalar values. Malformed UTF-8,
 * overlong forms, surrogate code points and NUL bytes are rejected.
 */
tr_error_t tr_text_check(const char *text, size_t length, uint32_t *scalars);

/* Validate a canonical 16 kHz mono PCM16 WAV envelope. Audio is not copied. */
tr_error_t tr_wav_check(const uint8_t *data, size_t length, tr_wav_info_t *info);

/* Lowercase SHA-256 used to bind an upload to its admitted descriptor. */
bool tr_sha256_hex(const void *data, size_t length, char out[TR_SHA256_SIZE]);

void tr_core_init(tr_core_t *core);

/*
 * Admit or replay one operation. A matching durable row returns the original
 * job/generation without a provider claim. A different payload for the same
 * operation is a conflict. A full table rejects new work with TR_E_BUSY and
 * never evicts deduplication evidence.
 */
tr_error_t tr_core_admit(tr_core_t *core, const tr_capability_t *capability,
                         const tr_admission_t *request, int64_t now,
                         tr_job_t *result);

/*
 * Accept the upload for an admitted job. The WAV envelope, byte count and
 * SHA-256 must match admission. Repeating the same upload is idempotent and
 * does not advance the state or imply another provider claim.
 */
tr_error_t tr_core_upload(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, const uint8_t *wav,
                          size_t length, int64_t now);

/* Mark an uploaded job as running before an external claim/result. */
tr_error_t tr_core_running(tr_core_t *core, const tr_capability_t *capability,
                           const char *operation_id, const char *job_id,
                           uint32_t generation, int64_t now);

/*
 * Apply a terminal result. DONE requires both source and translation text;
 * FAILED/UNKNOWN may carry only a valid source so a failed translation does
 * not erase recognized text. Results never overwrite a terminal job, and a
 * cancelled job rejects every late result.
 */
tr_error_t tr_core_result(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, const char *source_text,
                          size_t source_length, const char *target_text,
                          size_t target_length, tr_state_t terminal,
                          int64_t now);

/* Cancel is idempotent for a cancelled job. DONE/FAILED/UNKNOWN cannot cancel. */
tr_error_t tr_core_cancel(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, int64_t now);

/*
 * Invalidate the local generation on UI exit. Active jobs become CANCELLED and
 * their text flags are cleared; later results cannot match the new generation.
 */
uint32_t tr_core_invalidate(tr_core_t *core, int64_t now);

tr_error_t tr_core_get(const tr_core_t *core, const char *operation_id,
                       tr_job_t *result);

uint32_t tr_core_count(const tr_core_t *core);
uint32_t tr_core_event_count(const tr_core_t *core);
tr_event_t tr_core_event_at(const tr_core_t *core, uint32_t index);

const char *tr_error_name(tr_error_t error);
const char *tr_state_name(tr_state_t state);
const char *tr_event_name(tr_event_t event);
