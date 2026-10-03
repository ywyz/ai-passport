/**
 * travel_cfg.h - private settings store for the travel app (D1).
 *
 * Namespace "trvcfg" on the records NVS partition. Wi-Fi credentials, the
 * server HTTPS base URL, the installed CA (PEM), the device token and the
 * storage slot pointer live here; binding codes are NEVER stored and
 * passwords/tokens are never logged (logredaction is enforced by only ever
 * comparing digests).
 */
#ifndef TRAVEL_CFG_H
#define TRAVEL_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "passport_kv.h"
#include "passport_pack.h"
#include "passport_events.h"

#define TRAVEL_CFG_SSID_MAX 33
#define TRAVEL_CFG_PASS_MAX 65
#define TRAVEL_CFG_URL_MAX 192
#define TRAVEL_CFG_CA_MAX 4096

/* fixed slot config: two FAT resource generations (contract section 10) */
#define TRAVEL_SLOT_COUNT 2

struct travel_cfg {
    char wifi_ssid[TRAVEL_CFG_SSID_MAX];
    char wifi_pass[TRAVEL_CFG_PASS_MAX];
    char server_url[TRAVEL_CFG_URL_MAX];
    char device_token[96];   /* scoped, revocable credential; PRIVATE */
    char ca_pem[TRAVEL_CFG_CA_MAX];
    uint32_t active_slot;    /* 0 = cache_a, 1 = cache_b */
    char active_pack_id[PASSPORT_ID_MAX];
    uint32_t active_revision;
    char server_epoch[40];
    char pull_cursor[96];
    bool provisioned;
};

int travel_cfg_open(void);
/* set_* persist before returning 0; failures return nonzero without change */
int travel_cfg_set_wifi(const char *ssid, const char *password);
int travel_cfg_set_server(const char *url, const char *ca_pem);
int travel_cfg_set_credentials(const char *token, const char *server_epoch);
int travel_cfg_set_active_pack(uint32_t slot, const char *pack_id,
                               uint32_t revision);
int travel_cfg_set_pull_cursor(const char *cursor);
void travel_cfg_forget_network(void);

const struct travel_cfg *travel_cfg_get(void);

#endif /* TRAVEL_CFG_H */
