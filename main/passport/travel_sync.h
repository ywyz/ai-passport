/**
 * travel_sync.h - device sync + resource activation pipeline (D1c).
 *
 * Loop enabled (A02/A03/A04/A05/A15 foundations):
 *   bind (SoftAP-configured code -> exchange -> physical confirm -> scoped
 *   revocable token) -> catalog -> manifest -> streamed per-file staging on
 *   the inactive FAT slot -> digest + glyph checks -> active pointer swap.
 *   Offline saves persist before success; HTTP pushes only after durable
 *   journal writes; acks clear pending entries only.
 *
 * TLS/time bootstrap (PRD §9):
 *   the provisioning form supplies a browser-checked epoch (user_set) used
 *   once for initial time; all requests validate the CA, hostname and
 *   validity window; redirects are DISABLED so Authorization never leaves
 *   the configured origin; server_time replaces user_set after contact.
 */
#ifndef TRAVEL_SYNC_H
#define TRAVEL_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct travel_sync_view {
    bool bound;
    bool server_ok;
    enum travel_backup {
        TRAVEL_BACKUP_UNKNOWN,
        TRAVEL_BACKUP_PENDING,
        TRAVEL_BACKUP_COMPLETE,
        TRAVEL_BACKUP_UNCONFIGURED,
        TRAVEL_BACKUP_FAILED,
    } backup;
    enum travel_pack {
        TRAVEL_PACK_NONE = 0,
        TRAVEL_PACK_QUEUED,
        TRAVEL_PACK_DOWNLOADING,
        TRAVEL_PACK_READY,
        TRAVEL_PACK_FAILED,
    } pack;
    bool provisioned;
};

const struct travel_sync_view *travel_sync_view(void);

/* run one pipeline cycle (blocks; call from the worker task only):
 * returns 0 when the device is idle/ready, > 0 when config may need retry,
 * and nonzero codes on failures; cancellation is cooperative through
 * travel_sync_cancel(). */
int travel_sync_cycle(void);
void travel_sync_cancel(void);
/* manual retry (P02 action) */
void travel_sync_request_now(void);

#endif /* TRAVEL_SYNC_H */
