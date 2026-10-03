/**
 * travel_prov.h - time-limited SoftAP provisioning (D1, approved plan).
 *
 * - SoftAP SSID "Passport-D1-<rand>" with a per-session WPA2 password that
 *   is shown ONLY on the device screen (never logged, never stored).
 * - Local pages: form for SSID/password, server HTTPS URL, the binding
 *   code from the server website, and a browser-supplied date/time marked
 *   `user_set` for the FIRST TLS validation (PRD section 9); server_time
 *   supersedes it after the first authenticated contact.
 * - Hotspot stops after PASSPORT_PROV_TTL_MS or a successful apply; prior
 *   usable settings stay in NVS until the replacement connection verifies.
 * - Annual limits: one request at a time; the local server has no origins
 *   to the remote website (no cross-origin control).
 */
#ifndef TRAVEL_PROV_H
#define TRAVEL_PROV_H

#include <stdbool.h>

/* provisioning session state bits for the UI (P02/P03 views) */
enum travel_prov_state {
    PROV_IDLE = 0,
    PROV_HOTSPOT_RUNNING,
    PROV_APPLYING,
    PROV_DONE,
    PROV_FAILED,
};

#define PASSPORT_PROV_TTL_MS (5u * 60u * 1000u)

/* Start the temporary hotspot; returns state for the P03 page. */
int travel_prov_start(void);
/* stop hotspot (cancel by user or TTL) */
void travel_prov_stop(void);
enum travel_prov_state travel_prov_state(void);
/* 0 = unset (UI shows placeholder), else seconds of validity left */
unsigned long travel_prov_remaining_ms(void);

#endif /* TRAVEL_PROV_H */
