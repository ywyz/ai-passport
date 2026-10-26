#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "suite_translation_core.h"

/*
 * Strict, bounded JSON protocol for the translation service.
 *
 * This module has no ESP-IDF, LVGL, network, storage, clock or task
 * dependency. It validates the complete response document, rejects duplicate
 * object keys, malformed UTF-8, escaped NUL, lone surrogates, overflow and
 * trailing bytes, and copies only the bounded source/target text into
 * caller-owned buffers. Text is never clipped: a value that does not fit is
 * rejected instead.
 *
 * The backend response is a closed contract with a few known fields, but
 * unknown fields are tolerated for forward compatibility. Every object's keys
 * are still checked for duplicates within that object, including inside
 * unknown nested values; sibling/ancestor objects may reuse a key name.
 * Standard JSON whitespace is accepted everywhere the grammar permits it.
 * Non-DONE states may carry a bounded fixed error code (for example
 * CANCELLED/SHUTDOWN or EXPIRED/AUDIO_EXPIRED); DONE still requires no error,
 * FAILED still requires one, and text/identity invariants are unchanged.
 *
 * The transport uses a caller-owned JSON buffer of SUITE_TR_PROTOCOL_JSON_MAX
 * bytes plus one NUL. A response whose declared body is larger is rejected by
 * the transport before reading; this parser independently enforces the same
 * bound. The backend currently serializes with ensure_ascii=False. A future
 * change that escapes every non-ASCII byte can exceed this cap; that is an
 * explicit rejection, not a claim that every valid JSON escaping fits.
 */

#define SUITE_TR_PROTOCOL_JSON_MAX 8192U
#define SUITE_TR_PROTOCOL_TEXT_CAP (TR_TEXT_MAX_BYTES + 1U)
#define SUITE_TR_PROTOCOL_ERROR_CODE_MAX 32U
#define SUITE_TR_PROTOCOL_PROVIDER_MAX 16U
#define SUITE_TR_PROTOCOL_AUDIO_MIN (44U + TR_WAV_MIN_DATA_BYTES)
#define SUITE_TR_PROTOCOL_DEPTH_MAX 8U
#define SUITE_TR_PROTOCOL_KEYS_MAX 64U

typedef enum {
    SUITE_TR_PROTOCOL_DIRECTION_EN_ZH = 1,
    SUITE_TR_PROTOCOL_DIRECTION_ZH_EN = 2
} suite_translation_protocol_direction_t;

typedef enum {
    SUITE_TR_PROTOCOL_STATE_UNKNOWN = 0,
    SUITE_TR_PROTOCOL_STATE_ADMITTED,
    SUITE_TR_PROTOCOL_STATE_RUNNING,
    SUITE_TR_PROTOCOL_STATE_DONE,
    SUITE_TR_PROTOCOL_STATE_FAILED,
    SUITE_TR_PROTOCOL_STATE_CANCELLED,
    SUITE_TR_PROTOCOL_STATE_EXPIRED,
    SUITE_TR_PROTOCOL_STATE_REMOTE_UNKNOWN,
    SUITE_TR_PROTOCOL_STATE_TEXT_LOST
} suite_translation_protocol_state_t;

typedef enum {
    SUITE_TR_PROTOCOL_OK = 0,
    SUITE_TR_PROTOCOL_E_ARGUMENT,
    SUITE_TR_PROTOCOL_E_JSON,
    SUITE_TR_PROTOCOL_E_UTF8,
    SUITE_TR_PROTOCOL_E_DUPLICATE,
    SUITE_TR_PROTOCOL_E_NUL,
    SUITE_TR_PROTOCOL_E_SURROGATE,
    SUITE_TR_PROTOCOL_E_NUMBER,
    SUITE_TR_PROTOCOL_E_VALUE,
    SUITE_TR_PROTOCOL_E_OVERFLOW,
    SUITE_TR_PROTOCOL_E_LIMIT,
    SUITE_TR_PROTOCOL_E_DEPTH,
    SUITE_TR_PROTOCOL_E_TYPE,
    SUITE_TR_PROTOCOL_E_MISSING,
    SUITE_TR_PROTOCOL_E_SCHEMA,
    SUITE_TR_PROTOCOL_E_ID,
    SUITE_TR_PROTOCOL_E_SHA256,
    SUITE_TR_PROTOCOL_E_DIRECTION,
    SUITE_TR_PROTOCOL_E_STATE,
    SUITE_TR_PROTOCOL_E_TEXT,
    SUITE_TR_PROTOCOL_E_TEXT_BYTES,
    SUITE_TR_PROTOCOL_E_TEXT_SCALARS,
    SUITE_TR_PROTOCOL_E_IDENTITY,
    SUITE_TR_PROTOCOL_E_PROVIDER,
    SUITE_TR_PROTOCOL_E_TRAILING
} suite_translation_protocol_error_t;

typedef struct {
    bool ready;
    char provider[SUITE_TR_PROTOCOL_PROVIDER_MAX + 1U];
} suite_translation_protocol_ready_t;

/*
 * Parsed view. source/target are caller-owned buffers. A NULL buffer discards
 * that text while still validating it; a non-NULL buffer must have room for
 * TR_TEXT_MAX_BYTES bytes and the terminator (2049 bytes). Lengths exclude the
 * terminator. The transport uses this view for admission, upload, status and
 * cancel responses; the caller may choose to discard text on cancel.
 */
typedef struct {
    char operation_id[TR_ID_SIZE];
    char job_id[TR_ID_SIZE];
    uint32_t generation;
    suite_translation_protocol_direction_t direction;
    suite_translation_protocol_state_t state;
    uint32_t audio_bytes;
    uint32_t source_bytes;
    uint32_t target_bytes;
    int64_t updated_at;
    char error[SUITE_TR_PROTOCOL_ERROR_CODE_MAX + 1U];
    bool has_error;
    bool text_lost;
    char *source;
    size_t source_cap;
    size_t source_length;
    bool has_source;
    char *target;
    size_t target_cap;
    size_t target_length;
    bool has_target;
} suite_translation_protocol_view_t;

/* Expected identity after admission. job_id is NULL for admission. */
typedef struct {
    const char *operation_id;
    const char *job_id;
    uint32_t generation;
    suite_translation_protocol_direction_t direction;
    uint32_t audio_bytes;
} suite_translation_protocol_identity_t;

void suite_translation_protocol_view_init(suite_translation_protocol_view_t *view,
                                          char *source, size_t source_cap,
                                          char *target, size_t target_cap);

/*
 * Parse the whole response; `length` is the exact body length (no NUL
 * required). On error, the view must not be used. The admission parser checks
 * operation_id/direction/audio_bytes and returns the server job_id/generation.
 */
suite_translation_protocol_error_t suite_translation_protocol_parse_admission(
    const uint8_t *json, size_t length, const char *operation_id,
    suite_translation_protocol_direction_t direction, uint32_t audio_bytes,
    suite_translation_protocol_view_t *view);

/* Exact op/job/direction/generation/audio identity. */
suite_translation_protocol_error_t suite_translation_protocol_parse_view(
    const uint8_t *json, size_t length,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view);

/*
 * Cancel response parser. The backend advances generation on cancellation, so
 * a CANCELLED view may carry expected generation or expected+1. The state must
 * be CANCELLED; a local cancellation always wins over a late result.
 */
suite_translation_protocol_error_t suite_translation_protocol_parse_cancel_view(
    const uint8_t *json, size_t length,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view);

/* {"schema":1,"ready":true,"provider":"mimo"} with unknown fields tolerated. */
suite_translation_protocol_error_t suite_translation_protocol_parse_ready(
    const uint8_t *json, size_t length,
    suite_translation_protocol_ready_t *ready);

/* Fixed {"error":"CODE"} error body; unknown fields tolerated. */
suite_translation_protocol_error_t suite_translation_protocol_parse_error(
    const uint8_t *json, size_t length, char *code, size_t code_cap);

/* Build the exact admission request body. */
suite_translation_protocol_error_t suite_translation_protocol_build_admission(
    char *out, size_t cap, const char *operation_id,
    suite_translation_protocol_direction_t direction, const char *audio_sha256,
    uint32_t audio_bytes, size_t *out_length);

bool suite_translation_protocol_id_valid(const char *id);
bool suite_translation_protocol_sha256_valid(const char *sha256);
const char *suite_translation_protocol_error_name(
    suite_translation_protocol_error_t error);
const char *suite_translation_protocol_state_name(
    suite_translation_protocol_state_t state);
const char *suite_translation_protocol_direction_source(
    suite_translation_protocol_direction_t direction);
const char *suite_translation_protocol_direction_target(
    suite_translation_protocol_direction_t direction);
