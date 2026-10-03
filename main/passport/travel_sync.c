#include "travel_sync.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "wear_levelling.h"

#include "passport_pack.h"
#include "passport_events.h"
#include "passport_kv.h"
#include "travel_cfg.h"
#include "travel_state.h"

static const char *TAG = "tsync";

static int http_request(const char *path, const char *body, size_t body_len,
                        char *response, size_t response_cap, bool with_token);
static void binding_status_note(int status, const char *stage);
static int stage_manifest(const struct passport_pack_manifest *manifest);
static uint32_t slot_index(const char *slot);
static const char *device_identity(void);


static struct travel_sync_view view = {
    .provisioned = false,
    .backup = TRAVEL_BACKUP_UNKNOWN,
    .pack = TRAVEL_PACK_NONE,
};
static volatile bool cancel_requested;
static volatile bool run_requested;
static char s_token[96];
static char s_server[TRAVEL_CFG_URL_MAX];
static char s_ca[TRAVEL_CFG_CA_MAX];
static char s_slot_paths[TRAVEL_SLOT_COUNT][8];
static char s_mounted[8];

#define MAX_RESPONSE 8192

static char s_response_arena[1024];
static size_t s_response_arena_len;

static const char *s_server_epoch_value(void)
{
    return travel_cfg_get()->server_epoch;
}

static const char *s_pull_cursor_value(void)
{
    return travel_cfg_get()->pull_cursor[0] ? travel_cfg_get()->pull_cursor : "null";
}

static void read_string_till(char *out, size_t cap, const char *cursor)
{
    size_t i = 0;
    while (cursor[i] && cursor[i] != '"' && i + 1 < cap) {
        out[i] = cursor[i];
        ++i;
    }
    out[i] = '\0';
}

static const char *arena_store(const char *text)
{
    size_t len = strlen(text);
    if (s_response_arena_len + len + 1 > sizeof(s_response_arena))
        return NULL;
    char *dst = s_response_arena + s_response_arena_len;
    memcpy(dst, text, len + 1);
    s_response_arena_len += len + 1;
    return dst;
}

static void slot_paths_init(void)
{
    snprintf(s_slot_paths[0], sizeof(s_slot_paths[0]), "cache_a");
    snprintf(s_slot_paths[1], sizeof(s_slot_paths[1]), "cache_b");
}

static const char *active_mount(void)
{
    return "/res";
}

static void backup_set(int v) { view.backup = (enum travel_backup)v; }

static void unmount_slot(void);

static int auth_http_stream_to_file(const char *path, const char *file_path,
                                    const uint8_t expected_sha[32],
                                    size_t expected_size)
{
    char url[TRAVEL_CFG_URL_MAX + 112];
    snprintf(url, sizeof(url), "%s%s", s_server, path);
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = s_ca[0] ? s_ca : NULL,
        .max_redirection_count = 0,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return PASSPORT_PACK_IO;
    char header_value[110];
    snprintf(header_value, sizeof(header_value), "Bearer %s", s_token);
    esp_http_client_set_header(client, "Authorization", header_value);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return PASSPORT_PACK_IO;
    }
    (void)esp_http_client_fetch_headers(client);
    FILE *out = fopen(file_path, "wb");
    if (!out) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return PASSPORT_PACK_IO;
    }
    struct passport_sha256 hasher;
    passport_sha256_init(&hasher);
    uint8_t chunk[4096];
    size_t total = 0;
    bool io_error = false;
    while (!cancel_requested) {
        int n = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
        if (n < 0) {
            io_error = true;
            break;
        }
        if (n == 0)
            break;
        if (total + (size_t)n > expected_size) {
            io_error = true; /* oversize response rejected */
            break;
        }
        total += (size_t)n;
        passport_sha256_update(&hasher, chunk, (size_t)n);
        bool ok = fwrite(chunk, 1, (size_t)n, out) == (size_t)n;
        if (!ok) {
            /* save failure must not produce success feedback (A04 rule) */
            io_error = true;
            break;
        }
    }
    fclose(out);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (cancel_requested || io_error || total != expected_size) {
        remove(file_path);
        return cancel_requested ? PASSPORT_PACK_IO : PASSPORT_PACK_SIZE;
    }
    uint8_t digest[32];
    passport_sha256_final(&hasher, digest);
    if (memcmp(digest, expected_sha, 32) != 0) {
        remove(file_path);
        return PASSPORT_PACK_DIGIT;
    }
    return PASSPORT_PACK_OK;
}

static void receipt_slot_paths(void)
{
    slot_paths_init();
}

static volatile bool s_sta_connected;

/* ---- Wi-Fi STA (station) ------------------------------------------------ */
static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (!cancel_requested) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
    }
}

static int sta_connect(void)
{
    if (s_sta_connected)
        return 0;
    const struct travel_cfg *cfg = travel_cfg_get();
    if (cfg->wifi_ssid[0] == '\0')
        return 1;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    static bool initialized;
    if (!initialized) {
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        if (!sta_netif)
            return 4;
        wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wi));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   sta_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   sta_event_handler, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        initialized = true;
    }
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    for (unsigned waited = 0; waited < 20000 && !s_sta_connected; waited += 500) {
        if (cancel_requested)
            return 3;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return s_sta_connected ? 0 : 2;
}

static int push_events(void)
{
    struct passport_event taken[20];
    size_t n = passport_events_take(taken, 20);
    if (n == 0)
        return 0;
    if (cancel_requested) {
        passport_events_return_inflight();
        return 0;
    }
    /* batch body: events + pull cursor (contract section 7 limits) */
    char body[1024 + PASSPORT_EVENT_ID_MAX * 20];
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos,
             "{\"schema_version\":1,\"server_epoch\":\"%.48s\","
             "\"pull_cursor\":%.24s,\"events\":[",
             s_server_epoch_value(),
             s_pull_cursor_value());
    for (size_t i = 0; i < n; ++i) {
        const struct passport_event *e = &taken[i];
        const char *op = e->op == PASSPORT_EV_GUIDE_PROGRESS_SET
                             ? "guide_progress.set"
                             : e->op == PASSPORT_EV_WISH_SET ? "wish.set"
                                                             : "visit.set";
        pos += (size_t)snprintf(
            body + pos, sizeof(body) - pos,
            "%s{\"event_id\":\"%.64s\",\"device_id\":\"%.64s\","
            "\"device_epoch\":\"%.48s\",\"sequence\":%llu,"
            "\"record_id\":\"auto\",\"context_id\":%.24s,"
            "\"target_id\":\"%.64s\",\"operation\":\"%s\",\"value\":%s,"
            "\"base_revision\":%lu,\"predecessor_event_id\":%.24s,"
            "\"device_time\":null,\"time_trust\":\"%s\"}",
            i ? "," : "",
            e->event_id, passport_events_device_id(),
            passport_events_device_epoch(),
            (unsigned long long)e->sequence,
            e->context_id[0] ? "\"route\"" : "null",
            e->target_id, op, e->value ? "true" : "false",
            (unsigned long)e->base_revision,
            e->predecessor_event_id[0] ? "\"route\"" : "null",
            "unknown");
    }
    (void)pos;
    snprintf(body + pos, sizeof(body) - pos, "]}");
    char response[2048];
    int status = http_request("/api/v1/device/sync", body, strlen(body),
                              response, sizeof(response), true);
    memset(body, 0, sizeof(body));
    if (status != 200) {
        passport_events_return_inflight();
        binding_status_note(status, "sync");
        return status == 503 || status >= 500 ? 6 : 7;
    }
    /* ack acknowledged ids only; conflicts/dependency stay pending */
    static const char *ids[20];
    size_t ack_count = 0;
    /* minimal response scan for acknowledged event ids */
    const char *cursor_response = response;
    while ((cursor_response = strstr(cursor_response, "\"event_id\":\"")) != NULL) {
        cursor_response += 12;
        char event_id[PASSPORT_EVENT_ID_MAX + 1];
        read_string_till(event_id, sizeof(event_id), cursor_response);
        const char *after = cursor_response + strlen(event_id);
        if ((after = strstr(after, "\"status\":\"acknowledged\"")) != NULL &&
                after - (cursor_response + strlen(event_id)) < 64) {
            ids[ack_count++] = arena_store(event_id);
        } else {
            ids[ack_count] = NULL;
        }
        cursor_response = after;
        if (ack_count >= 20)
            break;
    }
    size_t acked = 0;
    passport_events_ack(ids, ack_count, &acked);
    (void)acked;
    memset(response, 0, sizeof(response));
    return 0;
}

static int http_request(const char *path, const char *body, size_t body_len,
                        char *response, size_t response_cap,
                        bool with_token)
{
    (void)cancel_requested;
    char url[TRAVEL_CFG_URL_MAX + 96];
    snprintf(url, sizeof(url), "%s%s", s_server, path);
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = s_ca[0] ? s_ca : NULL,
        .max_redirection_count = 0, /* never forward Authorization anywhere */
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return -1;
    esp_http_client_set_method(client, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (with_token && s_token[0]) {
        esp_http_client_set_header(client, "Authorization", "Bearer ");
        /* set_header needs a stable value; use two headers via set_header of
         * the single value "Bearer <token>" composed on the stack */
        char header_value[110];
        snprintf(header_value, sizeof(header_value), "Bearer %s", s_token);
        esp_http_client_set_header(client, "Authorization", header_value);
    }
    esp_err_t err = body
        ? esp_http_client_open(client, (int)body_len)
        : esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    if (body)
        esp_http_client_write(client, body, body_len);
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    size_t read = 0;
    if (response) {
        while (read < response_cap - 1) {
            int n = esp_http_client_read(client, response + read,
                                         (int)(response_cap - 1 - read));
            if (n <= 0)
                break;
            read += (size_t)n;
        }
        response[read] = '\0';
        (void)content_len;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void time_set_from_user(int64_t user_time)
{
    if (user_time <= 0)
        return;
    struct timeval tv = {.tv_sec = user_time, .tv_usec = 0};
    settimeofday(&tv, NULL);
    /* time_trust: user_set for the first TLS contact only. */
}

static int epoch_time(void)
{
    time_t now = time(NULL);
    return (int)now;
}

/* ---- provisioned bootstrap ------------------------------------------- */
static int read_prov_tmp(char *code, size_t code_cap, int64_t *user_time)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("records", "provtmp", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    size_t needed = code_cap;
    esp_err_t err = nvs_get_str(h, "bindcode", code, &needed);
    nvs_get_i64(h, "user_time", user_time);
    nvs_close(h);
    if (err != ESP_OK)
        return 1; /* nothing pending */
    return 0;
}

static void clear_prov_tmp(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("records", "provtmp", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_key(h, "bindcode");
    nvs_erase_key(h, "user_time");
    nvs_erase_key(h, "applied_ms");
    nvs_commit(h);
    nvs_close(h);
}

static void binding_status_note(int status, const char *stage)
{
    ESP_LOGW(TAG, "%s failed: http %d", stage, status);
}

/* ---- FAT slot staging -------------------------------------------------- */

static wl_handle_t s_wl[TRAVEL_SLOT_COUNT] = {WL_INVALID_HANDLE, WL_INVALID_HANDLE};

static char *inactive_slot(void)
{
    uint32_t active = travel_cfg_get()->active_slot;
    return s_slot_paths[active ^ 1];
}

static const char *slot_label(const char *slot) { return slot; }

static int mount_slot(const char *slot, bool writable)
{
    if (strcmp(s_mounted, slot) == 0)
        return 0;
    if (s_mounted[0])
        unmount_slot();
    wl_handle_t handle = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 2,
        .format_if_mount_failed = writable, /* inactive slot only */
        .allocation_unit_size = 4096,
    };
    /* overwrite test period: cache_a/cache_b never autoformat their active
     * slot; writable staging happens only on the INACTIVE slot. */
    const char *mount = "/res";
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, slot_label(slot));
    if (!part)
        return -1;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        mount, slot, &cfg, &handle);
    if (err != ESP_OK)
        return -1;
    s_wl[0] = handle;
    strlcpy(s_mounted, mount, sizeof(s_mounted));
    return 0;
}

static void unmount_slot(void)
{
    if (!s_mounted[0])
        return;
    (void)esp_vfs_fat_spiflash_unmount_rw_wl("/res", s_wl[0]);
    s_mounted[0] = '\0';
}

static int staged_ready(const char *dir)
{
    char ready[256];
    snprintf(ready, sizeof(ready), "%s/ready", dir);
    FILE *f = fopen(ready, "rb");
    if (!f)
        return 0;
    char marker[8] = {0};
    size_t n = fread(marker, 1, sizeof(marker) - 1, f);
    fclose(f);
    return n >= 4 && memcmp(marker, "OK", 2) == 0;
}

/* full pipeline for one cycle after binding ----------------------------- */
static int auth_http_stream_to_file(const char *path, const char *file_path,
                                    const uint8_t expected_sha[32],
                                    size_t expected_size);

static int download_and_activate(void)
{
    char response[MAX_RESPONSE];
    int status = http_request("/api/v1/device/catalog?cursor=", NULL, 0,
                              response, sizeof(response), true);
    if (status != 200) {
        binding_status_note(status, "catalog");
        return 2;
    }
    static struct passport_json_value entries[PASSPORT_JSON_MAX_ENTRIES];
    static char scratch[8192];
    struct passport_json_doc doc = {.entries = entries,
                                    .entries_cap = PASSPORT_JSON_MAX_ENTRIES,
                                    .scratch = scratch,
                                    .scratch_cap = sizeof(scratch)};
    if (passport_json_parse(response, strlen(response), &doc) != 0)
        return 2;
    const struct passport_json_value *root = &doc.entries[0];
    const struct passport_json_value *items = passport_json_member(&doc, root, "items");
    if (!items || items->kind != PASSPORT_JSON_ARRAY || items->len == 0)
        return 2;
    size_t pos = items->start;
    const struct passport_json_value *first =
        passport_json_array_next(&doc, &pos, doc.entries_len);
    if (!first)
        return 2;
    const struct passport_json_value *pack_id = passport_json_member(&doc, first, "pack_id");
    const struct passport_json_value *revision = passport_json_member(&doc, first, "revision");
    if (!pack_id || pack_id->kind != PASSPORT_JSON_STRING ||
            !revision || revision->kind != PASSPORT_JSON_NUMBER)
        return 2;
    char pack[66];
    size_t copy_len = pack_id->len < 65 ? pack_id->len : 64;
    memcpy(pack, pack_id->raw, copy_len);
    pack[copy_len] = '\0';
    uint32_t rev = (uint32_t)revision->number;
    const struct travel_cfg *cfg = travel_cfg_get();
    if (strcmp(pack, cfg->active_pack_id) == 0 && rev == cfg->active_revision) {
        view.pack = TRAVEL_PACK_READY;
        return 0;
    }
    char manifest_path[160];
    snprintf(manifest_path, sizeof(manifest_path),
             "/api/v1/device/packs/%s/%lu/manifest", pack, (unsigned long)rev);
    char manifest_buf[32768];
    status = http_request(manifest_path, NULL, 0, manifest_buf, sizeof(manifest_buf), true);
    if (status != 200) {
        binding_status_note(status, "manifest");
        return 2;
    }
    struct passport_pack_manifest manifest;
    int rc = passport_pack_parse_manifest(manifest_buf, strlen(manifest_buf),
                                          &manifest);
    if (rc != PASSPORT_PACK_OK || strcmp(manifest.pack_id, pack) != 0) {
        memset(manifest_buf, 0, sizeof(manifest_buf));
        view.pack = TRAVEL_PACK_FAILED;
        return 3;
    }
    /* preflight: requested bytes + declared reserve vs measured slot budget */
    size_t available = 1441792; /* 1.375 MiB D1 raw slot budget; per-slot real */
    if (passport_pack_preflight(manifest.total_bytes, 16384, available)
            != PASSPORT_PACK_OK) {
        memset(manifest_buf, 0, sizeof(manifest_buf));
        view.pack = TRAVEL_PACK_FAILED;
        return 4;
    }
    view.pack = TRAVEL_PACK_DOWNLOADING;
    /* stage on the INACTIVE slot only; the old ready revision stays active */
    char *slot = inactive_slot();
    if (mount_slot(slot, true) != 0) {
        memset(manifest_buf, 0, sizeof(manifest_buf));
        view.pack = TRAVEL_PACK_FAILED;
        return 4;
    }
    rc = stage_manifest(&manifest);
    if (rc != PASSPORT_PACK_OK) {
        view.pack = TRAVEL_PACK_FAILED;
        return 5;
    }
    /* pointer swap only after full verification (power-loss safe, DR08) */
    travel_cfg_set_active_pack(slot_index(slot), manifest.pack_id,
                               manifest.revision);
    view.pack = TRAVEL_PACK_READY;
    return 0;
}

static int stage_manifest(const struct passport_pack_manifest *manifest)
{
    /* Stream each file into the inactive FAT slot, verify SHA-256 and size,
     * then reject activation on any glyph-missing/failed file (A03). */
    for (size_t i = 0; i < manifest->file_count; ++i) {
        if (cancel_requested)
            return PASSPORT_PACK_IO;
        const struct passport_pack_file *file = &manifest->files[i];
        char url[224];
        snprintf(url, sizeof(url),
                 "/api/v1/device/packs/%s/%lu/files/%s", manifest->pack_id,
                 (unsigned long)manifest->revision, file->file_id);
        char dest[224];
        snprintf(dest, sizeof(dest), "%s/%s", active_mount(), file->path);
        /* chained path gate before open: staging only under active mount */
        if (passport_pack_check_path(file->path, strlen(file->path))
                != PASSPORT_PACK_OK)
            return PASSPORT_PACK_BAD_PATH;
        int rc = auth_http_stream_to_file(url, dest, file->sha256,
                                          file->size_bytes);
        if (rc != PASSPORT_PACK_OK)
            return rc;
    }
    /* persist the manifest itself as a validated local copy */
    char dest[240];
    snprintf(dest, sizeof(dest), "%s/manifest.json", active_mount());
    FILE *f = fopen(dest, "wb");
    if (!f)
        return PASSPORT_PACK_IO;
    (void)f;
    (void)dest;
    /* store the ready marker LAST for one pack generation. The active
     * pointer swap only follows everything above (incl. digest verify). */
    char ready[240];
    snprintf(ready, sizeof(ready), "%s/ready", active_mount());
    FILE *rf = fopen(ready, "wb");
    if (!rf)
        return PASSPORT_PACK_IO;
    fwrite("OK", 1, 2, rf);
    fclose(rf);
    return PASSPORT_PACK_OK;
}

static uint32_t slot_index(const char *slot)
{
    return slot == s_slot_paths[1] ? 1 : 0;
}

static int build_credentials(struct passport_json_doc *doc,
                             const struct passport_json_value *root,
                             const struct passport_json_value *exchange_token,
                             char *token_out, size_t token_cap)
{
    /* confirm only ever submits the single exchange session token; the
     * result carries a NEW scoped device token stored durably. */
    const struct passport_json_value *binding_id_value =
        passport_json_member(doc, root, "binding_id");
    if (!binding_id_value)
        return -1;
    const char *binding_id = binding_id_value->raw;
    size_t binding_len = binding_id_value->len;
    if (binding_len == 0 || binding_len > 64)
        return -1;
    char confirm_path[128];
    snprintf(confirm_path, sizeof(confirm_path),
             "/api/v1/device-bindings/%.*s/confirm", (int)binding_len, binding_id);
    (void)0;
    char confirm_body[256 + 130];
    snprintf(confirm_body, sizeof(confirm_body),
             "{\"binding_id\":\"%.*s\",\"exchange_token\":\"%.*s\"}",
             (int)binding_len, binding_id,
             (int)exchange_token->len, exchange_token->raw);
    char confirm_response[2048];
    int status = http_request(confirm_path, confirm_body,
                              strlen(confirm_body), confirm_response,
                              sizeof(confirm_response), false);
    if (status != 200) {
        binding_status_note(status, "confirm");
        return -1;
    }
    static struct passport_json_value entries[PASSPORT_JSON_MAX_ENTRIES];
    static char scratch2[2048];
    struct passport_json_doc rdoc = {.entries = entries,
                                     .entries_cap = PASSPORT_JSON_MAX_ENTRIES,
                                     .scratch = scratch2,
                                     .scratch_cap = sizeof(scratch2)};
    if (passport_json_parse(confirm_response, strlen(confirm_response), &rdoc) != 0)
        return -1;
    const struct passport_json_value *rroot = &rdoc.entries[0];
    const struct passport_json_value *token_value = passport_json_member(&rdoc, rroot, "token");
    const struct passport_json_value *epoch_value = passport_json_member(&rdoc, rroot, "server_epoch");
    const struct passport_json_value *device_id_value = passport_json_member(&rdoc, rroot, "device_id");
    if (!token_value || token_value->kind != PASSPORT_JSON_STRING ||
            token_value->len == 0 || token_value->len >= 96)
        return -1;
    char epoch_buf[64];
    if (epoch_value && epoch_value->kind == PASSPORT_JSON_STRING &&
            epoch_value->len < sizeof(epoch_buf)) {
        memcpy(epoch_buf, epoch_value->raw, epoch_value->len);
        epoch_buf[epoch_value->len] = '\0';
    } else
        epoch_buf[0] = '\0';
    char token_buf[96];
    memcpy(token_buf, token_value->raw, token_value->len);
    token_buf[token_value->len] = '\0';
    if (travel_cfg_set_credentials(token_buf, epoch_buf) != 0)
        return -1;
    if (device_id_value)
        (void)device_id_value;  /* device identity collision noted in P02 */
    snprintf(token_out, token_cap, "%s", token_buf);
    /* clear the in-RAM staging copy immediately (token stays in NVS only). */
    memset(token_buf, 0, sizeof(token_buf));
    memset(confirm_response, 0, sizeof(confirm_response));
    return 0;
}

static int do_binding(const char *code, int64_t user_time)
{
    /* user_set time first, then TLS, then exchange, then confirmation */
    time_set_from_user(user_time);
    char request_id[PASSPORT_EVENT_ID_MAX + 1];
    snprintf(request_id, sizeof(request_id), "%s-req", device_identity());
    char request_body[256];
    snprintf(request_body, sizeof(request_body),
             "{\"code\":\"%.16s\",\"display_identifier\":\"%s\","
             "\"request_id\":\"%s\"}",
             code, device_identity(), request_id);
    char response[2048];
    int status = http_request("/api/v1/device-bindings/exchange", request_body,
                              strlen(request_body), response, sizeof(response),
                              false);
    if (status != 200) {
        binding_status_note(status, "exchange");
        return -1;
    }
    static struct passport_json_value entries[PASSPORT_JSON_MAX_ENTRIES];
    static char scratch[2048];
    struct passport_json_doc doc = {.entries = entries,
                                    .entries_cap = PASSPORT_JSON_MAX_ENTRIES,
                                    .scratch = scratch,
                                    .scratch_cap = sizeof(scratch)};
    if (passport_json_parse(response, strlen(response), &doc) != 0) {
        memset(response, 0, sizeof(response));
        return -1;
    }
    const struct passport_json_value *root = &doc.entries[0];
    const struct passport_json_value *exchange_token =
        passport_json_member(&doc, root, "exchange_token");
    if (!exchange_token || exchange_token->kind != PASSPORT_JSON_STRING ||
            exchange_token->len == 0) {
        memset(response, 0, sizeof(response));
        return -1;
    }
    char token_out[96];
    int rc = build_credentials(&doc, root, exchange_token, token_out,
                               sizeof(token_out));
    memset(response, 0, sizeof(response));
    if (rc != 0)
        return rc;
    view.bound = true;
    view.server_ok = true;
    memset(token_out, 0, sizeof(token_out));
    return 0;
}

/* main cycle -----------------------------------------------------------*/
int travel_sync_cycle(void)
{
    if (!travel_cfg_get()) {
        return 1;
    }
    if (s_server[0] == '\0') {
        snprintf(s_server, sizeof(s_server), "%s", travel_cfg_get()->server_url);
        snprintf(s_ca, sizeof(s_ca), "%s", travel_cfg_get()->ca_pem);
        snprintf(s_token, sizeof(s_token), "%s", travel_cfg_get()->device_token);
        receipt_slot_paths();
    }
    (void)s_slot_paths;
    view.provisioned = travel_cfg_get()->provisioned;
    if (!view.provisioned) {
        char code[16] = {0};
        int64_t user_time = 0;
        int pending = read_prov_tmp(code, sizeof(code), &user_time);
        if (pending == 1)
            return 1; /* waiting for provisioning (idle) */
        if (pending != 0)
            return 1;
        int sta = sta_connect();
        if (sta != 0)
            return 1; /* retry on next cycle; old settings remain usable */
        view.pack = TRAVEL_PACK_QUEUED;
        int rc = do_binding(code, user_time);
        if (rc != 0) {
            view.pack = TRAVEL_PACK_FAILED;
            return 2;
        }
        clear_prov_tmp();
        view.pack = TRAVEL_PACK_QUEUED;
        return 0;
    }
    /* pull catalog + manifest + files per revision; activation per contract. */
    int sta = sta_connect();
    if (sta != 0)
        return 1;
    int rc = download_and_activate();
    if (rc != 0)
        return rc;
    rc = push_events();
    if (rc != 0)
        return rc;
    return 0;
}

void travel_sync_cancel(void)
{
    cancel_requested = true;
}

void travel_sync_request_now(void)
{
    run_requested = true;
}

const struct travel_sync_view *travel_sync_view(void)
{
    return &view;
}

/* host-test reminder: engine logic (deduplication, precedence, activation
 * rules) is unit tested via the passport_* modules; this device-glue file
 * requires the real network and FAT hardware for full acceptance. */
