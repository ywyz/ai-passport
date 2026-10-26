#pragma once

/*
 * Pure-C numeric snapshot of the first transport-level TLS failure.
 *
 * This header intentionally has no ESP-IDF includes so the owned HTTP
 * transport, worker view and UI can share the same fixed-width evidence
 * without adding a layering dependency. Values are copied once before
 * esp_tls_conn_destroy() and contain no URL, token, certificate, audio or
 * plaintext.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SUITE_TR_TLS_STAGE_NONE = 0,
    SUITE_TR_TLS_STAGE_INIT_NULL = 1,
    SUITE_TR_TLS_STAGE_ASYNC1 = 2,
    SUITE_TR_TLS_STAGE_STATE1 = 3,
    SUITE_TR_TLS_STAGE_STATE_FAIL = 4,
    SUITE_TR_TLS_STAGE_TCP_SOCKFD = 5,
    SUITE_TR_TLS_STAGE_TCP_SELECT = 6,
    SUITE_TR_TLS_STAGE_TCP_GETSOCKOPT = 7,
    SUITE_TR_TLS_STAGE_TCP_SO_ERROR = 8,
    SUITE_TR_TLS_STAGE_ASYNC2 = 9,
    SUITE_TR_TLS_STAGE_POST_STATE = 10,
    SUITE_TR_TLS_STAGE_TCP_STUCK = 11,
    SUITE_TR_TLS_STAGE_HANDSHAKE_LOOP = 12
} suite_translation_tls_stage_t;

typedef enum {
    SUITE_TR_TLS_SYS_NONE = 0,
    SUITE_TR_TLS_SYS_LOCAL_ERRNO = 1,
    SUITE_TR_TLS_SYS_SO_ERROR = 2,
    SUITE_TR_TLS_SYS_SDK = 3
} suite_translation_tls_sys_source_t;

typedef struct {
    bool valid;
    uint8_t stage;
    uint8_t sys_source;
    int32_t local_errno;
    int32_t so_error;
    int32_t sdk_sys_errno;
    int32_t sys_error;       /* priority: SO_ERROR, SDK system, local errno */
    int32_t esp_err_code;    /* esp_tls_get_and_clear_last_error return */
    int32_t tls_code;        /* library code, signed as returned by SDK */
    uint32_t verify_flags;   /* certificate verification flags */
    size_t free_bytes;       /* INTERNAL|8BIT before destroy */
    size_t largest_bytes;    /* INTERNAL|8BIT before destroy */
} suite_translation_tls_diag_t;
