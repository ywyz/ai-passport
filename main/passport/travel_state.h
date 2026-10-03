/**
 * travel_state.h - travel UI state model, navigation + pack/sync views.
 * Pure logic, host-testable (no LVGL/IDF includes).
 */
#ifndef TRAVEL_STATE_H
#define TRAVEL_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* page ids (PRD section 3, D1 subset) */
enum travel_page {
    TRAVEL_P00_HOME = 0,
    TRAVEL_P01_LAUNCHER,
    TRAVEL_P02_SYNC,
    TRAVEL_P03_SETTINGS,
    TRAVEL_P04_MENU,        /* long-OK action menu */
    TRAVEL_P05_DETAIL,      /* labeled fixture content detail (test data) */
    TRAVEL_PAGE_COUNT
};

/* raw key events resolved by passport_keys */
enum travel_key {
    TRAVEL_KEY_NONE = 0,
    TRAVEL_EVT_PRESS,
    TRAVEL_EVT_RELEASE,
    TRAVEL_EVT_CLICK,
    TRAVEL_EVT_HOLD_START,
    TRAVEL_EVT_REPEAT,
};

/* application-owned battery in top-right corner (ai-guide default) */
struct travel_battery_view {
    int soc;             /* -1 = unavailable → suppressed */
    bool present;
};

struct travel_state {
    enum travel_page page;
    uint8_t menu_index;      /* selection within list pages (P01..P04) */
    uint8_t menu_count;
    bool battery_shown;
    /* P02 views */
    bool wifi_connected;
    bool bound;
    bool server_ok;
    uint32_t pending_events;
    enum travel_backup_view { BACKUP_UNKNOWN = 0, BACKUP_PENDING, BACKUP_COMPLETE,
                              BACKUP_FAILED, BACKUP_UNCONFIGURED } backup;
    /* P05 detail */
    bool stop_completed;
    bool pack_ready;
    char stop_label[64];
};

void travel_state_init(struct travel_state *self);

/* handle one cooked key event; returns true when the event was consumed */
bool travel_state_key(struct travel_state *self, char event, int button);

/* queries for the UI layer */
uint8_t travel_state_menu_size(const struct travel_state *self);
const char *travel_state_page_title(int page);
bool travel_state_page_is_list(int page);

uint8_t travel_state_menu_index(const struct travel_state *self);
/* labelled helpers for host tests */
bool travel_state_allow_destructive(const struct travel_state *self);

#endif /* TRAVEL_STATE_H */
