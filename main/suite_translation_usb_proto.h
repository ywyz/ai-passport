#pragma once
#include "suite_translation_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Strict, transport-independent USB provisioning protocol.
 *
 * One bounded JSON line requests one translation configuration write. Frames
 * must be UTF-8, contain exactly the documented keys once, and match the
 * request ID used in the acknowledgement. The acknowledgement never repeats
 * the origin or token, and the runner never echoes input bytes.
 */

#define SUITE_TRANSLATION_USB_FRAME_MAX 1024U
#define SUITE_TRANSLATION_USB_WINDOW_MAX_MS 60000U
#define SUITE_TRANSLATION_USB_WINDOW_DEFAULT_MS 60000U

typedef enum {
    SUITE_TRANSLATION_USB_STATUS_OK = 0,
    SUITE_TRANSLATION_USB_STATUS_INVALID,
    SUITE_TRANSLATION_USB_STATUS_BUSY,
    SUITE_TRANSLATION_USB_STATUS_STORAGE,
    SUITE_TRANSLATION_USB_STATUS_UNVERIFIED
} suite_translation_usb_status_t;

typedef struct {
    char request_id[33];
    char origin[SUITE_TRANSLATION_ORIGIN_MAX + 1U];
    char token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
} suite_translation_usb_request_t;

typedef enum {
    SUITE_TRANSLATION_USB_PARSE_IGNORE = 0,
    SUITE_TRANSLATION_USB_PARSE_REQUEST,
    SUITE_TRANSLATION_USB_PARSE_INVALID
} suite_translation_usb_parse_t;

typedef enum {
    SUITE_TRANSLATION_USB_FRAME_NONE = 0,
    SUITE_TRANSLATION_USB_FRAME_READY,
    SUITE_TRANSLATION_USB_FRAME_OVERSIZE
} suite_translation_usb_frame_t;

typedef struct {
    char line[SUITE_TRANSLATION_USB_FRAME_MAX + 1U];
    size_t used;
    bool discarding;
} suite_translation_usb_framer_t;

typedef enum {
    SUITE_TRANSLATION_USB_STEP_NONE = 0,
    SUITE_TRANSLATION_USB_STEP_APPLY,
    SUITE_TRANSLATION_USB_STEP_REPLY
} suite_translation_usb_step_t;

typedef struct {
    bool open, write_consumed;
    uint32_t payload_tag;
    char request_id[33];
    suite_translation_usb_status_t status;
    bool verified;
} suite_translation_usb_session_t;

void suite_translation_usb_framer_reset(suite_translation_usb_framer_t *framer);
suite_translation_usb_frame_t suite_translation_usb_framer_push(
    suite_translation_usb_framer_t *framer, char byte);

suite_translation_usb_parse_t suite_translation_usb_parse(
    const char *line, size_t bytes, suite_translation_usb_request_t *request);

void suite_translation_usb_session_reset(suite_translation_usb_session_t *session);
suite_translation_usb_step_t suite_translation_usb_session_feed(
    suite_translation_usb_session_t *session, const char *line, size_t bytes,
    suite_translation_usb_request_t *request);
void suite_translation_usb_session_complete(suite_translation_usb_session_t *session,
                                            suite_translation_usb_status_t status,
                                            bool verified);

/* Returns the serialized length, or 0 for an invalid request ID/capacity. */
size_t suite_translation_usb_ack(char *out, size_t capacity, const char *request_id,
                                 suite_translation_usb_status_t status, bool verified);
const char *suite_translation_usb_status_name(suite_translation_usb_status_t status);

/* Portable runner used by the firmware window and host tests. It reads at most
   one apply request, acknowledges it, and exits; invalid/duplicate frames are
   acknowledged without writing. */
typedef struct {
    int (*read)(void *ctx, void *buffer, size_t capacity, uint32_t timeout_ms);
    int (*write)(void *ctx, const void *buffer, size_t bytes, uint32_t timeout_ms);
    int64_t (*now_ms)(void *ctx);
    void *ctx;
} suite_translation_usb_transport_t;

typedef suite_translation_apply_t (*suite_translation_usb_apply_fn)(
    const char *origin, const char *token, void *ctx);

typedef enum {
    SUITE_TRANSLATION_USB_RUN_COMPLETED = 0,
    SUITE_TRANSLATION_USB_RUN_CANCELLED,
    SUITE_TRANSLATION_USB_RUN_TIMEOUT,
    SUITE_TRANSLATION_USB_RUN_IO_ERROR,
    SUITE_TRANSLATION_USB_RUN_INVALID_ARGUMENT
} suite_translation_usb_run_t;

suite_translation_usb_run_t suite_translation_usb_run(
    const suite_translation_usb_transport_t *transport,
    suite_translation_usb_apply_fn apply, void *apply_ctx,
    bool (*cancelled)(void *ctx), void *cancel_ctx, uint32_t window_ms);
