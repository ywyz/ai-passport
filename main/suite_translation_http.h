#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "suite_translation_protocol.h"
#include "suite_translation_tls_diag.h"

/*
 * Worker-owned, synchronous, bounded HTTPS transport for the translation
 * service.
 *
 * Design constraints:
 * - Call only from one worker task. The transport keeps no global request
 *   state except a two-slot static DNS wait table; concurrent calls are not
 *   supported.
 * - No LVGL, audio-device, scratch/Flash or Tasks ledger access. Audio is
 *   streamed through a caller reader callback; nothing retains the WAV.
 * - The caller supplies the already validated origin/token and owns the text
 *   buffers. `json` may be NULL to request transport-allocated per-call
 *   response storage. With NULL the transport parses and validates the
 *   bounded response headers first, then allocates exactly Content-Length + 1
 *   bytes (never more than SUITE_TR_HTTP_JSON_CAP) only when a body is needed,
 *   and zeroes and frees it before returning. A non-NULL `json` is
 *   caller-owned: it is zeroed after each call and never freed or resized by
 *   the transport. The transport does not load configuration and does not log
 *   URL, token, audio or text.
 * - One suite_net take/give per HTTP attempt. TLS is closed before the network
 *   owner is released, and only this worker frees it.
 * - DNS uses lwIP's asynchronous resolver; TCP/TLS uses esp_tls non-blocking
 *   calls. Every wait polls the caller cancellation callback and the absolute
 *   deadline. No cross-task cancel/free and no redirects or retries.
 * - This is not esp_http_client. Its synchronous header/body loops reuse an
 *   idle timeout per iteration, and its cancel API reconnects with no thread
 *   safety; neither can provide an absolute worker-owned bound.
 *
 * HTTP framing is intentionally small and strict: one HTTP/1.1 request,
 * Connection: close, exactly one Content-Length, no Transfer-Encoding, identity
 * Content-Encoding only, no informational or redirected response, and a body
 * no larger than SUITE_TR_PROTOCOL_JSON_MAX. Chunked/close-delimited bodies are
 * rejected. The reverse proxy must preserve Content-Length and must not
 * compress these responses.
 *
 * Failure behavior: the response storage, view text buffers and ready output
 * are cleared; caller buffers and stack audio/header/reader/line buffers are
 * zeroized. info keeps the numeric status and a parsed fixed remote_error for
 * diagnostics. A parsed view's state may be EXPIRED (backend AUDIO_EXPIRED) or
 * CANCELLED with error SHUTDOWN; the worker must treat both as terminal.
 * Status polling interval and the 60-second record deadline belong to the
 * caller; this module performs one bounded GET per call.
 */

#define SUITE_TR_HTTP_JSON_CAP (SUITE_TR_PROTOCOL_JSON_MAX + 1U)
#define SUITE_TR_HTTP_AUDIO_CHUNK 512U
#define SUITE_TR_HTTP_AUDIO_MIN SUITE_TR_PROTOCOL_AUDIO_MIN
#define SUITE_TR_HTTP_AUDIO_MAX TR_WAV_MAX_BYTES
#define SUITE_TR_HTTP_CONNECT_TIMEOUT_MS 100
#define SUITE_TR_HTTP_POLL_TICKS 1U
#define SUITE_TR_HTTP_DNS_SLOTS 2U
#define SUITE_TR_HTTP_HEADER_BYTES_MAX 4096U
#define SUITE_TR_HTTP_HEADER_LINE_MAX 512U
#define SUITE_TR_HTTP_HEADERS_MAX 32U

typedef bool (*suite_translation_http_cancel_fn)(void *ctx);

/*
 * Read finalized, caller-owned scratch WAV bytes without retaining the file.
 * Return the number of bytes written to buffer (0..capacity). A short read is
 * allowed and continues at the next offset; zero or more than capacity aborts.
 * The transport requests at most SUITE_TR_HTTP_AUDIO_CHUNK bytes per call.
 */
typedef size_t (*suite_translation_http_audio_read_fn)(void *ctx,
                                                       uint32_t offset,
                                                       uint8_t *buffer,
                                                       size_t capacity);

typedef struct {
    const char *origin;
    const char *token;
    uint8_t *json;
    size_t json_cap;
} suite_translation_http_t;

typedef struct {
    int64_t deadline_us; /* absolute esp_timer time */
    suite_translation_http_cancel_fn cancel;
    void *cancel_ctx;
} suite_translation_http_call_t;

typedef enum {
    SUITE_TR_HTTP_OK = 0,
    SUITE_TR_HTTP_E_ARGUMENT,
    SUITE_TR_HTTP_E_BUFFER,
    SUITE_TR_HTTP_E_OFFLINE,
    SUITE_TR_HTTP_E_TIME,
    SUITE_TR_HTTP_E_NET_BUSY,
    SUITE_TR_HTTP_E_DNS,
    SUITE_TR_HTTP_E_TLS,
    SUITE_TR_HTTP_E_IO,
    SUITE_TR_HTTP_E_CANCELLED,
    SUITE_TR_HTTP_E_DEADLINE,
    SUITE_TR_HTTP_E_HTTP_REDIRECT,
    SUITE_TR_HTTP_E_HTTP_AUTH,
    SUITE_TR_HTTP_E_HTTP_NOT_FOUND,
    SUITE_TR_HTTP_E_HTTP_CONFLICT,
    SUITE_TR_HTTP_E_HTTP_BUSY,
    SUITE_TR_HTTP_E_HTTP_QUOTA,
    SUITE_TR_HTTP_E_HTTP_LIMIT,
    SUITE_TR_HTTP_E_HTTP_BAD_REQUEST,
    SUITE_TR_HTTP_E_HTTP_UNAVAILABLE,
    SUITE_TR_HTTP_E_HTTP_STATUS,
    SUITE_TR_HTTP_E_CONTENT_TYPE,
    SUITE_TR_HTTP_E_CONTENT_ENCODING,
    SUITE_TR_HTTP_E_TRANSFER_ENCODING,
    SUITE_TR_HTTP_E_LENGTH,
    SUITE_TR_HTTP_E_LIMIT,
    SUITE_TR_HTTP_E_OVERSIZE,
    SUITE_TR_HTTP_E_TRUNCATED,
    SUITE_TR_HTTP_E_PROTOCOL,
    SUITE_TR_HTTP_E_IDENTITY,
    SUITE_TR_HTTP_E_NOT_READY
} suite_translation_http_result_t;

typedef struct {
    int http_status;
    suite_translation_protocol_error_t protocol_error;
    char remote_error[SUITE_TR_PROTOCOL_ERROR_CODE_MAX + 1U];
    /* True when request bytes may have reached the server and the outcome is
       uncertain. The caller must not repeat a POST/PUT or paid operation. */
    bool ambiguous;
    uint32_t request_body_bytes;
    uint32_t response_body_bytes;
    suite_translation_tls_diag_t tls_diag;
} suite_translation_http_info_t;

/*
 * Validate borrowed origin/token pointers and buffer sizes. The origin and
 * token must remain alive for every call. This repeats the header-safety
 * part of the config validation so malformed values cannot inject a request.
 * `json` may be NULL when `json_cap` is at least SUITE_TR_HTTP_JSON_CAP; each
 * call then allocates exactly Content-Length + 1 bytes after parsing the
 * bounded headers, only when the body is needed, and releases that storage
 * after parsing. An allocation failure after request bytes may already have
 * been sent sets info->ambiguous, like other late transport failures; the
 * caller must not repeat a POST/PUT. A non-NULL `json` is caller-owned and is
 * only zeroed, never freed or resized by the transport.
 */
suite_translation_http_result_t suite_translation_http_init(
    suite_translation_http_t *http, const char *origin, const char *token,
    uint8_t *json, size_t json_cap);

/* GET /suite/v1/translations/ready. Takes suite_net only. */
suite_translation_http_result_t suite_translation_http_ready(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    suite_translation_protocol_ready_t *ready,
    suite_translation_http_info_t *info);

/* POST /suite/v1/translations. One attempt; no replay. */
suite_translation_http_result_t suite_translation_http_admit(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call, const char *operation_id,
    suite_translation_protocol_direction_t direction,
    const char *audio_sha256, uint32_t audio_bytes,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info);

/*
 * PUT /suite/v1/translations/{job_id}/audio. Streams exactly
 * expected->audio_bytes from the reader once, in at most 512-byte chunks.
 * A failure after request bytes may have been sent sets info->ambiguous. The
 * caller must not re-upload; poll status instead when the outcome is unclear.
 */
suite_translation_http_result_t suite_translation_http_upload(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_http_audio_read_fn read, void *read_ctx,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info);

/* GET /suite/v1/translations/{job_id}. Exact identity is enforced. */
suite_translation_http_result_t suite_translation_http_status(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info);

/*
 * DELETE /suite/v1/translations/{job_id}, best effort within the call
 * deadline. The response may advance generation by one and must be CANCELLED.
 * The caller's local cancellation wins even if this returns a transport error.
 */
suite_translation_http_result_t suite_translation_http_cancel(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info);

const char *suite_translation_http_result_name(
    suite_translation_http_result_t result);
