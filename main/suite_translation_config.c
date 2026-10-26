#include "suite_translation_config.h"
#include "suite_tasks_core.h"

#include "esp_partition.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stddef.h>
#include <string.h>

#define CONFIG_MAGIC 0x314e5254U /* "TRN1" little-endian */
#define CONFIG_NAMESPACE "translation"
#define CONFIG_KEY "config"

typedef struct {
    uint32_t magic, version, schema;
    char origin[SUITE_TRANSLATION_ORIGIN_MAX + 1U];
    char token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
    uint32_t crc;
} translation_blob_t;

static uint32_t blob_crc(const translation_blob_t *blob)
{
    return task_crc32(~0U, blob, offsetof(translation_blob_t, crc));
}

bool suite_translation_config_origin_valid(const char *origin)
{
    return origin && task_origin_valid(origin);
}

bool suite_translation_config_token_valid(const char *token)
{
    return token && task_token_valid(token);
}

/* Read the existing taskkeys material and initialize taskcfg without erase,
   deinit or key generation. Idempotent on a boot where Tasks opened it first. */
static esp_err_t ensure_taskcfg(void)
{
    nvs_sec_cfg_t key = {0};
    esp_err_t result = ESP_FAIL;
    const esp_partition_t *keys = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, "taskkeys");
    if (!keys) goto done;
    if (nvs_flash_read_security_cfg(keys, &key) != ESP_OK) goto done;
    result = nvs_flash_secure_init_partition("taskcfg", &key);
done:
    mbedtls_platform_zeroize(&key, sizeof(key));
    return result;
}

static esp_err_t open_encrypted(int mode, nvs_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ensure_taskcfg();
    if (result != ESP_OK) return result;
    return nvs_open_from_partition("taskcfg", CONFIG_NAMESPACE, mode, handle);
}

static bool blob_valid(const translation_blob_t *blob)
{
    return blob->magic == CONFIG_MAGIC && blob->version == SUITE_TRANSLATION_BLOB_VERSION &&
        blob->schema == SUITE_TRANSLATION_CONFIG_SCHEMA &&
        memchr(blob->origin, 0, sizeof(blob->origin)) &&
        memchr(blob->token, 0, sizeof(blob->token)) &&
        suite_translation_config_origin_valid(blob->origin) &&
        suite_translation_config_token_valid(blob->token) &&
        blob->crc == blob_crc(blob);
}

static bool blob_equal(const translation_blob_t *a, const translation_blob_t *b)
{
    return a->magic == b->magic && a->version == b->version && a->schema == b->schema &&
        a->crc == b->crc && !strcmp(a->origin, b->origin) && !strcmp(a->token, b->token);
}

static bool read_blob(nvs_handle_t handle, translation_blob_t *blob, bool *present)
{
    size_t bytes = sizeof(*blob);
    esp_err_t result = nvs_get_blob(handle, CONFIG_KEY, blob, &bytes);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        *present = false;
        memset(blob, 0, sizeof(*blob));
        return true;
    }
    if (result != ESP_OK || bytes != sizeof(*blob)) return false;
    *present = true;
    return true;
}

/* The translation capability must never equal either existing Tasks token.
   Missing Tasks keys are fine; any unreadable or oversized value fails closed
   because distinctness cannot then be proven. */
static bool tokens_distinct(const char *candidate, bool *distinct)
{
    nvs_handle_t handle;
    char token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
    char write_token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
    *distinct = false;
    memset(token, 0, sizeof(token));
    memset(write_token, 0, sizeof(write_token));
    esp_err_t opened = nvs_open_from_partition("taskcfg", "tasks", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        *distinct = true;
        return true;
    }
    if (opened != ESP_OK) return false;
    bool ok = true;
    size_t n = sizeof(token);
    esp_err_t result = nvs_get_str(handle, "token", token, &n);
    if (result == ESP_ERR_NVS_NOT_FOUND) token[0] = 0;
    else if (result != ESP_OK) ok = false;
    n = sizeof(write_token);
    if (ok) {
        result = nvs_get_str(handle, "write_token", write_token, &n);
        if (result == ESP_ERR_NVS_NOT_FOUND) write_token[0] = 0;
        else if (result != ESP_OK) ok = false;
    }
    nvs_close(handle);
    if (ok) *distinct = strcmp(candidate, token) && strcmp(candidate, write_token);
    mbedtls_platform_zeroize(token, sizeof(token));
    mbedtls_platform_zeroize(write_token, sizeof(write_token));
    return ok;
}

suite_translation_config_status_t suite_translation_config_get(suite_translation_config_t *out)
{
    if (!out) return SUITE_TRANSLATION_CONFIG_INVALID;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t opened = open_encrypted(NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return SUITE_TRANSLATION_CONFIG_UNCONFIGURED;
    if (opened != ESP_OK) return SUITE_TRANSLATION_CONFIG_KEY;
    translation_blob_t blob;
    bool present = false;
    bool read = read_blob(handle, &blob, &present);
    nvs_close(handle);
    if (!read) {
        mbedtls_platform_zeroize(&blob, sizeof(blob));
        return SUITE_TRANSLATION_CONFIG_STORAGE;
    }
    if (!present) {
        mbedtls_platform_zeroize(&blob, sizeof(blob));
        return SUITE_TRANSLATION_CONFIG_UNCONFIGURED;
    }
    if (!blob_valid(&blob)) {
        mbedtls_platform_zeroize(&blob, sizeof(blob));
        return SUITE_TRANSLATION_CONFIG_INVALID;
    }
    strcpy(out->origin, blob.origin);
    strcpy(out->token, blob.token);
    out->schema = blob.schema;
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    return SUITE_TRANSLATION_CONFIG_OK;
}

/* Read back through a fresh handle after any ambiguous set/commit result. */
static suite_translation_apply_t confirm(const translation_blob_t *candidate,
                                         const translation_blob_t *previous,
                                         bool previous_present)
{
    nvs_handle_t handle;
    if (open_encrypted(NVS_READONLY, &handle) != ESP_OK)
        return SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN;
    translation_blob_t current;
    bool present = false;
    bool read = read_blob(handle, &current, &present);
    nvs_close(handle);
    if (!read) {
        mbedtls_platform_zeroize(&current, sizeof(current));
        return SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN;
    }
    suite_translation_apply_t result = SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN;
    if (present && blob_valid(&current) && blob_equal(&current, candidate))
        result = SUITE_TRANSLATION_APPLY_OK;
    else if (previous_present && present && !memcmp(&current, previous, sizeof(current)))
        result = SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;
    else if (!previous_present && !present)
        result = SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;
    mbedtls_platform_zeroize(&current, sizeof(current));
    return result;
}

suite_translation_apply_t suite_translation_config_apply(const char *origin,
                                                        const char *token,
                                                        bool *changed)
{
    if (changed) *changed = false;
    if (!suite_translation_config_origin_valid(origin) ||
        !suite_translation_config_token_valid(token))
        return SUITE_TRANSLATION_APPLY_INVALID;

    /* The encrypted taskcfg partition must exist before the Tasks namespace is
       inspected; a fresh boot may not have initialized it yet. */
    if (ensure_taskcfg() != ESP_OK) return SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;

    bool distinct = false;
    if (!tokens_distinct(token, &distinct))
        return SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;
    if (!distinct) return SUITE_TRANSLATION_APPLY_INVALID;

    nvs_handle_t handle;
    esp_err_t opened = open_encrypted(NVS_READWRITE, &handle);
    if (opened != ESP_OK) return SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;

    translation_blob_t previous;
    bool previous_present = false;
    bool read = read_blob(handle, &previous, &previous_present);
    if (!read) {
        nvs_close(handle);
        mbedtls_platform_zeroize(&previous, sizeof(previous));
        return SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;
    }

    translation_blob_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.magic = CONFIG_MAGIC;
    candidate.version = SUITE_TRANSLATION_BLOB_VERSION;
    candidate.schema = SUITE_TRANSLATION_CONFIG_SCHEMA;
    strcpy(candidate.origin, origin);
    strcpy(candidate.token, token);
    candidate.crc = blob_crc(&candidate);

    if (previous_present && blob_valid(&previous) && blob_equal(&previous, &candidate)) {
        nvs_close(handle);
        mbedtls_platform_zeroize(&candidate, sizeof(candidate));
        mbedtls_platform_zeroize(&previous, sizeof(previous));
        return SUITE_TRANSLATION_APPLY_UNCHANGED;
    }

    if (nvs_set_blob(handle, CONFIG_KEY, &candidate, sizeof(candidate)) == ESP_OK)
        (void)nvs_commit(handle);
    nvs_close(handle);

    suite_translation_apply_t outcome = confirm(&candidate, &previous, previous_present);
    mbedtls_platform_zeroize(&candidate, sizeof(candidate));
    mbedtls_platform_zeroize(&previous, sizeof(previous));
    if (outcome == SUITE_TRANSLATION_APPLY_OK && changed) *changed = true;
    return outcome;
}
