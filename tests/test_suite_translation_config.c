/* Real translation configuration against an in-memory encrypted NVS model. */
#include "suite_translation_config.h"
#include "suite_tasks_core.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NS_TASKS 1
#define NS_TASKOPS 2
#define NS_TASKVOICE 3
#define NS_TRANSLATION 4
#define NS_COUNT 5
#define ENTRY_MAX 32
#define VALUE_MAX 512

typedef struct {
    uint16_t id;
    char key[17];
    int kind; /* 1=string, 2=blob, 3=u32 */
    uint8_t data[VALUE_MAX];
    size_t length;
    bool present;
} entry_t;

static entry_t entries[ENTRY_MAX];
static unsigned entry_count;
static bool ns_exists[NS_COUNT];
static esp_partition_t partition = {.size = 0x3000};
static bool keys_partition = true, read_cfg_fail, secure_init_fail;
static bool initialized;
static unsigned init_count;
static bool set_fail, commit_fail, corrupt_set, string_read_fail;
static unsigned set_count, commit_count, erase_count;
static entry_t pending[ENTRY_MAX];
static unsigned pending_count;
static bool dirty;

static void reset_model(void)
{
    memset(entries, 0, sizeof(entries)); entry_count = 0;
    memset(ns_exists, 0, sizeof(ns_exists));
    memset(pending, 0, sizeof(pending)); pending_count = 0;
    dirty = false;
    keys_partition = true; read_cfg_fail = secure_init_fail = false;
    initialized = false; init_count = 0;
    set_fail = commit_fail = corrupt_set = string_read_fail = false;
    set_count = commit_count = erase_count = 0;
}

static int namespace_id(const char *space)
{
    if (!strcmp(space, "tasks")) return NS_TASKS;
    if (!strcmp(space, "taskops")) return NS_TASKOPS;
    if (!strcmp(space, "taskvoice")) return NS_TASKVOICE;
    if (!strcmp(space, "translation")) return NS_TRANSLATION;
    return 0;
}

static entry_t *find_entry(int id, const char *key)
{
    for (unsigned i = 0; i < entry_count; i++)
        if (entries[i].id == id && !strcmp(entries[i].key, key)) return &entries[i];
    return NULL;
}

static entry_t *add_entry(int id, const char *key)
{
    assert(entry_count < ENTRY_MAX && strlen(key) <= 16);
    entry_t *entry = &entries[entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = (uint16_t)id;
    strcpy(entry->key, key);
    return entry;
}

static void put_blob(int id, const char *key, const void *data, size_t length)
{
    entry_t *entry = find_entry(id, key);
    if (!entry) entry = add_entry(id, key);
    assert(length <= VALUE_MAX);
    entry->kind = 2; entry->length = length; entry->present = true;
    memcpy(entry->data, data, length);
    ns_exists[id] = true;
}
static void put_string(int id, const char *key, const char *value)
{
    put_blob(id, key, value, strlen(value) + 1);
    find_entry(id, key)->kind = 1;
}

/* NVS surface used by the real configuration source. */
esp_err_t nvs_open_from_partition(const char *part, const char *space, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(part, "taskcfg"));
    int id = namespace_id(space);
    if (!id) return ESP_ERR_INVALID_ARG;
    if (!initialized) return 0x1101; /* ESP_ERR_NVS_NOT_INITIALIZED */
    if (!ns_exists[id]) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        ns_exists[id] = true;
    }
    *handle = id;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *bytes)
{
    if (string_read_fail) return ESP_FAIL;
    entry_t *entry = find_entry(handle, key);
    if (!entry || !entry->present) return ESP_ERR_NVS_NOT_FOUND;
    if (entry->kind != 1 || *bytes < entry->length) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, entry->data, entry->length);
    *bytes = entry->length;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *bytes)
{
    entry_t *entry = find_entry(handle, key);
    if (!entry || !entry->present) return ESP_ERR_NVS_NOT_FOUND;
    if (entry->kind != 2 || *bytes != entry->length) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, entry->data, entry->length);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t bytes)
{
    set_count++;
    if (set_fail) return ESP_FAIL;
    if (!dirty) {
        memcpy(pending, entries, sizeof(entries));
        pending_count = entry_count;
        dirty = true;
    }
    entry_t *entry = find_entry(handle, key);
    if (!entry) entry = add_entry(handle, key);
    entry->kind = 2; entry->length = bytes; entry->present = true;
    memcpy(entry->data, value, bytes);
    if (corrupt_set && bytes) entry->data[bytes / 2] ^= 0xa5;
    ns_exists[handle] = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    commit_count++;
    if (commit_fail) {
        memcpy(entries, pending, sizeof(entries));
        entry_count = pending_count;
        dirty = false;
        return ESP_FAIL;
    }
    dirty = false;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle; (void)key; erase_count++; return ESP_FAIL;
}
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    (void)handle; (void)key; (void)out; return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle; (void)key; (void)value; return ESP_FAIL;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{
    (void)type; (void)subtype;
    assert(!strcmp(name, "taskkeys"));
    return keys_partition ? &partition : NULL;
}
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *part, nvs_sec_cfg_t *cfg)
{
    (void)part;
    if (read_cfg_fail) return ESP_FAIL;
    memset(cfg, 0, sizeof(*cfg));
    return ESP_OK;
}
esp_err_t nvs_flash_secure_init_partition(const char *name, nvs_sec_cfg_t *cfg)
{
    (void)cfg;
    assert(!strcmp(name, "taskcfg"));
    if (secure_init_fail) return ESP_FAIL;
    initialized = true;
    init_count++;
    return ESP_OK;
}
void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)

typedef struct {
    uint32_t magic, version, schema;
    char origin[SUITE_TRANSLATION_ORIGIN_MAX + 1U];
    char token[SUITE_TRANSLATION_TOKEN_MAX + 1U];
    uint32_t crc;
} blob_t;

static void seed_tasks(void)
{
    put_string(NS_TASKS, "origin", "https://tasks.example.test");
    put_string(NS_TASKS, "token", "tasks_read_token_0123456789abcdef");
    put_string(NS_TASKS, "write_token", "tasks_write_token_abcdef0123456789");
    for (unsigned i = 0; i < 8; i++) {
        uint8_t row[64];
        memset(row, (int)(0x40 + i), sizeof(row));
        char key[8];
        snprintf(key, sizeof(key), "op%u", i);
        put_blob(NS_TASKOPS, key, row, sizeof(row));
    }
    uint8_t voice[80];
    memset(voice, 0x5a, sizeof(voice));
    put_blob(NS_TASKVOICE, "intent", voice, sizeof(voice));
    put_string(NS_TASKS, "sentinel", "preserve-me");
}

static void check_seed_preserved(void)
{
    assert(!strcmp((char *)find_entry(NS_TASKS, "origin")->data, "https://tasks.example.test"));
    assert(!strcmp((char *)find_entry(NS_TASKS, "token")->data, "tasks_read_token_0123456789abcdef"));
    assert(!strcmp((char *)find_entry(NS_TASKS, "write_token")->data, "tasks_write_token_abcdef0123456789"));
    assert(!strcmp((char *)find_entry(NS_TASKS, "sentinel")->data, "preserve-me"));
    for (unsigned i = 0; i < 8; i++) {
        char key[8];
        snprintf(key, sizeof(key), "op%u", i);
        entry_t *row = find_entry(NS_TASKOPS, key);
        assert(row && row->length == 64);
        for (unsigned j = 0; j < row->length; j++) assert(row->data[j] == (uint8_t)(0x40 + i));
    }
    entry_t *voice = find_entry(NS_TASKVOICE, "intent");
    assert(voice && voice->length == 80);
    for (unsigned j = 0; j < voice->length; j++) assert(voice->data[j] == 0x5a);
    assert(erase_count == 0);
}

static const char *ORIGIN = "https://translate.example.test";
static const char *TOKEN = "translation_token_0123456789abcdef";

int main(void)
{
    suite_translation_config_t config;
    bool changed = false;

    /* First use on a fresh boot initializes encrypted taskcfg before the Tasks
       namespace lookup; the actual new write is APPLY_OK, not ambiguous. */
    reset_model();
    assert(!initialized);
    assert(suite_translation_config_apply(ORIGIN, TOKEN, &changed) ==
           SUITE_TRANSLATION_APPLY_OK);
    assert(initialized && init_count > 0 && changed);

    /* Unconfigured and closed validation. */
    reset_model();
    assert(suite_translation_config_get(&config) == SUITE_TRANSLATION_CONFIG_UNCONFIGURED);
    assert(suite_translation_config_apply("http://translate.example.test", TOKEN, &changed) ==
           SUITE_TRANSLATION_APPLY_INVALID);
    assert(suite_translation_config_apply(ORIGIN, "short", &changed) ==
           SUITE_TRANSLATION_APPLY_INVALID);
    assert(suite_translation_config_apply(ORIGIN, "translation_token_0123456789abcdef/", &changed) ==
           SUITE_TRANSLATION_APPLY_INVALID);
    keys_partition = false;
    assert(suite_translation_config_apply(ORIGIN, TOKEN, &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    keys_partition = true;
    assert(set_count == 0 && commit_count == 0 && erase_count == 0);

    /* Exact blob and preservation of Tasks namespaces. */
    reset_model();
    seed_tasks();
    unsigned prior_set = set_count, prior_commit = commit_count;
    assert(suite_translation_config_apply(ORIGIN, TOKEN, &changed) == SUITE_TRANSLATION_APPLY_OK);
    assert(changed && set_count == prior_set + 1 && commit_count == prior_commit + 1);
    assert(suite_translation_config_get(&config) == SUITE_TRANSLATION_CONFIG_OK);
    assert(!strcmp(config.origin, ORIGIN) && !strcmp(config.token, TOKEN));
    assert(config.schema == SUITE_TRANSLATION_CONFIG_SCHEMA);
    entry_t *stored = find_entry(NS_TRANSLATION, "config");
    assert(stored && stored->length == sizeof(blob_t));
    blob_t *blob = (blob_t *)stored->data;
    assert(blob->magic == 0x314e5254U && blob->version == 1 && blob->schema == 1);
    assert(!strcmp(blob->origin, ORIGIN) && !strcmp(blob->token, TOKEN));
    assert(blob->crc == task_crc32(~0U, blob, offsetof(blob_t, crc)));
    check_seed_preserved();

    /* Same configuration is idempotent. */
    prior_set = set_count; prior_commit = commit_count;
    changed = true;
    assert(suite_translation_config_apply(ORIGIN, TOKEN, &changed) ==
           SUITE_TRANSLATION_APPLY_UNCHANGED);
    assert(!changed && set_count == prior_set && commit_count == prior_commit);

    /* Capability must differ from both Tasks tokens. */
    prior_set = set_count;
    assert(suite_translation_config_apply(ORIGIN, "tasks_read_token_0123456789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_INVALID);
    assert(suite_translation_config_apply(ORIGIN, "tasks_write_token_abcdef0123456789", &changed) ==
           SUITE_TRANSLATION_APPLY_INVALID);
    assert(set_count == prior_set);

    /* Set failure preserves the previous blob and reports guaranteed no write. */
    set_fail = true;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_zzzz56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    set_fail = false;
    assert(suite_translation_config_get(&config) == SUITE_TRANSLATION_CONFIG_OK);
    assert(!strcmp(config.token, TOKEN));

    /* Commit failure also preserves the previous blob. */
    commit_fail = true;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_yyyy56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    commit_fail = false;
    assert(suite_translation_config_get(&config) == SUITE_TRANSLATION_CONFIG_OK);
    assert(!strcmp(config.token, TOKEN));

    /* Stored-but-different readback is ambiguous, not a claimed success. */
    corrupt_set = true;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_xxxx56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN);
    corrupt_set = false;

    /* Key/secure-init/task-token read failures fail closed without writing. */
    read_cfg_fail = true;
    prior_set = set_count;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_qqqq56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    assert(set_count == prior_set);
    read_cfg_fail = false;
    secure_init_fail = true;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_qqqq56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    assert(set_count == prior_set);
    secure_init_fail = false;
    string_read_fail = true;
    assert(suite_translation_config_apply(ORIGIN, "translation_token_qqqq56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    assert(set_count == prior_set);
    string_read_fail = false;
    keys_partition = false;
    assert(suite_translation_config_get(&config) == SUITE_TRANSLATION_CONFIG_KEY);
    assert(suite_translation_config_apply(ORIGIN, "translation_token_qqqq56789abcdef", &changed) ==
           SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE);
    keys_partition = true;

    check_seed_preserved();
    puts("translation config blob / idempotence / error injection / namespace preservation: PASS");
    return 0;
}
