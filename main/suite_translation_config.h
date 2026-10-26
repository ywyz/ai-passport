#pragma once
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Translation service configuration in the existing encrypted taskcfg
 * partition. One versioned blob holds origin, capability token and schema.
 * Reading never erases, deinitializes or generates NVS keys; writing preserves
 * every other namespace and fails closed on capacity or verification errors.
 */

#define SUITE_TRANSLATION_CONFIG_SCHEMA 1U
#define SUITE_TRANSLATION_ORIGIN_MAX 192U
#define SUITE_TRANSLATION_TOKEN_MIN 32U
#define SUITE_TRANSLATION_TOKEN_MAX 128U
#define SUITE_TRANSLATION_BLOB_VERSION 1U

typedef struct {
    char origin[SUITE_TRANSLATION_ORIGIN_MAX + 1U];
    char token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
    uint32_t schema;
} suite_translation_config_t;

typedef enum {
    SUITE_TRANSLATION_CONFIG_OK = 0,
    SUITE_TRANSLATION_CONFIG_UNCONFIGURED,
    SUITE_TRANSLATION_CONFIG_INVALID,
    SUITE_TRANSLATION_CONFIG_STORAGE,
    SUITE_TRANSLATION_CONFIG_KEY
} suite_translation_config_status_t;

typedef enum {
    SUITE_TRANSLATION_APPLY_OK = 0,
    SUITE_TRANSLATION_APPLY_UNCHANGED,
    SUITE_TRANSLATION_APPLY_INVALID,
    SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE,
    SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN
} suite_translation_apply_t;

/* Structural validation shared with the USB protocol parser. */
bool suite_translation_config_origin_valid(const char *origin);
bool suite_translation_config_token_valid(const char *token);

/* Read and validate the stored blob. The caller must zeroize the output. */
suite_translation_config_status_t suite_translation_config_get(suite_translation_config_t *out);

/* Validate, write, commit and reopen/read back. Identical input is idempotent.
   STORAGE_NO_WRITE means the previous blob is still authoritative; VERIFY_UNKNOWN
   means a commit/readback result is ambiguous and an idempotent retry is safe. */
suite_translation_apply_t suite_translation_config_apply(const char *origin,
                                                        const char *token,
                                                        bool *changed);
