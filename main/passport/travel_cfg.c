#include "travel_cfg.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include <stdio.h>

/* NVS namespace on the records partition; config + pointers per contract. */
#define NS "trvcfg"

static struct travel_cfg cfg;
static nvs_handle_t handle = 0;
static bool opened;

static int read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t needed = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &needed);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        dst[0] = '\0';
        return 1;
    }
    if (err != ESP_OK)
        return -1;
    if (needed >= cap)
        return -1;
    dst[needed] = '\0';
    return 0;
}

int travel_cfg_open(void)
{
    if (opened)
        return 0;
    esp_err_t err = nvs_flash_init_partition("records");
    nvs_open_from_partition("records", NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("trvcfg", "nvs_open failed: %d", err);
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    read_str(handle, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    read_str(handle, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    read_str(handle, "url", cfg.server_url, sizeof(cfg.server_url));
    read_str(handle, "token", cfg.device_token, sizeof(cfg.device_token));
    read_str(handle, "ca", cfg.ca_pem, sizeof(cfg.ca_pem));
    read_str(handle, "epoch", cfg.server_epoch, sizeof(cfg.server_epoch));
    read_str(handle, "cursor", cfg.pull_cursor, sizeof(cfg.pull_cursor));
    read_str(handle, "packid", cfg.active_pack_id, sizeof(cfg.active_pack_id));
    uint32_t value = 0;
    nvs_get_u32(handle, "slot", &value);
    cfg.active_slot = value < TRAVEL_SLOT_COUNT ? value : 0;
    nvs_get_u32(handle, "rev", &value);
    cfg.active_revision = value;
    nvs_get_u8(handle, "prov", (uint8_t *)&cfg.provisioned) ;
    opened = true;
    return 0;
}

static nvs_handle_t h(void) { return handle; }

int travel_cfg_set_wifi(const char *ssid, const char *password)
{
    if (strlen(ssid) >= TRAVEL_CFG_SSID_MAX || strlen(password) >= TRAVEL_CFG_PASS_MAX)
        return -1;
    if (nvs_set_str(h(), "ssid", ssid) != ESP_OK ||
            nvs_set_str(h(), "pass", password) != ESP_OK ||
            nvs_commit(h()) != ESP_OK)
        return -1;
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", password);
    return 0;
}

int travel_cfg_set_server(const char *url, const char *ca_pem)
{
    if (strlen(url) >= TRAVEL_CFG_URL_MAX || strlen(ca_pem) >= TRAVEL_CFG_CA_MAX)
        return -1;
    if (nvs_set_str(h(), "url", url) != ESP_OK ||
            nvs_set_str(h(), "ca", ca_pem) != ESP_OK ||
            nvs_commit(h()) != ESP_OK)
        return -1;
    snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", url);
    snprintf(cfg.ca_pem, sizeof(cfg.ca_pem), "%s", ca_pem);
    return 0;
}

int travel_cfg_set_credentials(const char *token, const char *server_epoch)
{
    if (!token || strlen(token) >= sizeof(cfg.device_token))
        return -1;
    if (nvs_set_str(h(), "token", token) != ESP_OK ||
            nvs_set_str(h(), "epoch", server_epoch ? server_epoch : "") != ESP_OK ||
            nvs_set_u8(h(), "prov", 1) != ESP_OK ||
            nvs_commit(h()) != ESP_OK)
        return -1;
    snprintf(cfg.device_token, sizeof(cfg.device_token), "%s", token);
    snprintf(cfg.server_epoch, sizeof(cfg.server_epoch), "%s",
             server_epoch ? server_epoch : "");
    cfg.provisioned = true;
    return 0;
}

int travel_cfg_set_active_pack(uint32_t slot, const char *pack_id,
                               uint32_t revision)
{
    if (slot >= TRAVEL_SLOT_COUNT || !pack_id ||
            strlen(pack_id) >= PASSPORT_ID_MAX)
        return -1;
    if (nvs_set_u32(h(), "slot", slot) != ESP_OK ||
            nvs_set_u32(h(), "rev", revision) != ESP_OK ||
            nvs_set_str(h(), "packid", pack_id) != ESP_OK ||
            nvs_commit(h()) != ESP_OK)
        return -1;
    cfg.active_slot = slot;
    snprintf(cfg.active_pack_id, sizeof(cfg.active_pack_id), "%s", pack_id);
    cfg.active_revision = revision;
    return 0;
}

int travel_cfg_set_pull_cursor(const char *cursor)
{
    if (!cursor || strlen(cursor) >= sizeof(cfg.pull_cursor))
        return -1;
    if (nvs_set_str(h(), "cursor", cursor) != ESP_OK || nvs_commit(h()) != ESP_OK)
        return -1;
    snprintf(cfg.pull_cursor, sizeof(cfg.pull_cursor), "%s", cursor);
    return 0;
}

void travel_cfg_forget_network(void)
{
    (void)nvs_erase_key(handle, "ssid");
    (void)nvs_erase_key(handle, "pass");
    (void)nvs_commit(handle);
    memset(cfg.wifi_ssid, 0, sizeof(cfg.wifi_ssid));
    memset(cfg.wifi_pass, 0, sizeof(cfg.wifi_pass));
}

const struct travel_cfg *travel_cfg_get(void)
{
    return opened ? &cfg : NULL;
}
