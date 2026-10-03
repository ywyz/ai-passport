#include "travel_state.h"

#include <string.h>

void travel_state_init(struct travel_state *self)
{
    memset(self, 0, sizeof(*self));
    self->page = TRAVEL_P00_HOME;
    self->backup = BACKUP_UNKNOWN;
    self->stop_label[0] = '\0';
}

static uint8_t menu_size(int page)
{
    switch (page) {
    case TRAVEL_P01_LAUNCHER: return 4; /* 向导/车票/同步/设置 */
    case TRAVEL_P02_SYNC: return 3;     /* 同步一次/返回/关闭 */
    case TRAVEL_P03_SETTINGS: return 3; /* 配网/清除网络/返回 */
    case TRAVEL_P04_MENU: return 3;     /* 返回/同步一次/关闭 */
    default: return 0;
    }
}

uint8_t travel_state_menu_size(const struct travel_state *self)
{
    return menu_size(self->page);
}

bool travel_state_page_is_list(int page)
{
    switch (page) {
    case TRAVEL_P01_LAUNCHER:
    case TRAVEL_P02_SYNC:
    case TRAVEL_P03_SETTINGS:
    case TRAVEL_P04_MENU:
        return true;
    default:
        return false;
    }
}

const char *travel_state_page_title(int page)
{
    switch (page) {
    case TRAVEL_P00_HOME: return "P00";
    case TRAVEL_P01_LAUNCHER: return "P01";
    case TRAVEL_P02_SYNC: return "P02";
    case TRAVEL_P03_SETTINGS: return "P03";
    case TRAVEL_P04_MENU: return "P04";
    case TRAVEL_P05_DETAIL: return "P05";
    default: return "?";
    }
}

static enum travel_page parent_of(int page)
{
    switch (page) {
    case TRAVEL_P01_LAUNCHER:
    case TRAVEL_P04_MENU:
    case TRAVEL_P05_DETAIL:
        return TRAVEL_P00_HOME;
    case TRAVEL_P02_SYNC:
    case TRAVEL_P03_SETTINGS:
        return TRAVEL_P01_LAUNCHER;
    default:
        return TRAVEL_P00_HOME;
    }
}

bool travel_state_allow_destructive(const struct travel_state *self)
{
    return self->page == TRAVEL_P03_SETTINGS;
}

/* Move list selection without wrapping (P01/P02/P03/P04: boundaries stop). */
static void move_selection(struct travel_state *self, int button)
{
    uint8_t count = menu_size(self->page);
    if (button == 0 && self->menu_index > 0)
        self->menu_index--;
    if (button == 1 && self->menu_index + 1 < count)
        self->menu_index++;
}

bool travel_state_key(struct travel_state *self, char event, int button)
{
    if (event == TRAVEL_KEY_NONE)
        return false;
    switch (self->page) {
    case TRAVEL_P00_HOME:
        if (button == 2 && (event == TRAVEL_EVT_CLICK ||
                            event == TRAVEL_EVT_HOLD_START)) {
            self->page = TRAVEL_P01_LAUNCHER; /* home + long-OK both open P01 */
            self->menu_index = 0;
            return true;
        }
        if (button == 0 && event == TRAVEL_EVT_CLICK) {
            self->page = TRAVEL_P05_DETAIL;
            return true;
        }
        return false;
    case TRAVEL_P01_LAUNCHER:
        if (event == TRAVEL_EVT_PRESS) {
            if (button == 0 || button == 1)
                return true; /* repeat scheduling handled by keys module */
            return false;
        }
        if (event == TRAVEL_EVT_REPEAT) {
            if (button == 0 || button == 1) {
                move_selection(self, button);
                return true;
            }
            return false;
        }
        if (button == 2 && event == TRAVEL_EVT_CLICK) {
            /* enter selected item: 0=guide detail(P05), 1=tickets,
             * 2=sync(P02), 3=settings(P03) */
            switch (self->menu_index) {
            case 0: self->page = TRAVEL_P05_DETAIL; break;
            case 1: self->page = TRAVEL_P02_SYNC; break;
            case 2: self->page = TRAVEL_P02_SYNC; break;
            case 3: self->page = TRAVEL_P03_SETTINGS; break;
            default: return false;
            }
            self->menu_index = 0;
            return true;
        }
        if (button == 2 && (event == TRAVEL_EVT_HOLD_START ||
                            event == TRAVEL_EVT_RELEASE)) {
            self->page = TRAVEL_P04_MENU;
            self->menu_index = 0;
            return true;
        }
        if (button == 0 && event == TRAVEL_EVT_RELEASE) {
            /* Back on launcher: press-and-release UP at boundary returns home */
            if (self->menu_index == 0)
                self->page = TRAVEL_P00_HOME;
            return true;
        }
        return false;
    case TRAVEL_P02_SYNC:
    case TRAVEL_P03_SETTINGS:
    case TRAVEL_P04_MENU:
        if (event == TRAVEL_EVT_PRESS)
            return button == 0 || button == 1;
        if (event == TRAVEL_EVT_REPEAT &&
                (button == 0 || button == 1)) {
            move_selection(self, button);
            return true;
        }
        if (button == 2 && (event == TRAVEL_EVT_HOLD_START ||
                            event == TRAVEL_EVT_RELEASE)) {
            if (self->page == TRAVEL_P04_MENU) {
                self->page = parent_of(TRAVEL_P01_LAUNCHER);
                self->menu_index = 0;
                return true;
            }
            self->page = TRAVEL_P04_MENU;
            self->menu_index = 0;
            return true;
        }
        if (button == 2 && event == TRAVEL_EVT_CLICK) {
            /* menu order: P04: BACK first, sync, CLOSE last (PRD P04);
             * P02: 0=sync once, 1=back, 2=close; P03: 0=provision,
             * 1=clear network (needs confirm), 2=back */
            uint8_t index = self->menu_index;
            if (self->page == TRAVEL_P02_SYNC && index == 1) {
                self->page = TRAVEL_P01_LAUNCHER;
                self->menu_index = 0;
                return true;
            }
            if (self->page == TRAVEL_P02_SYNC && index == 2) {
                self->page = TRAVEL_P00_HOME;
                self->menu_index = 0;
                return true;
            }
            if (self->page == TRAVEL_P03_SETTINGS && index == 2) {
                self->page = TRAVEL_P01_LAUNCHER;
                self->menu_index = 0;
                return true;
            }
            if (self->page == TRAVEL_P04_MENU) {
                if (index == 0) {
                    self->page = TRAVEL_P01_LAUNCHER;
                    self->menu_index = 0;
                    return true;
                }
                if (index == 2) {
                    self->page = TRAVEL_P00_HOME;
                    self->menu_index = 0;
                    return true;
                }
                return false;
            }
            return false;
        }
        /* explicit Back键: chunky UP press back to parent per page */
        if (button == 0 &&
                (event == TRAVEL_EVT_CLICK || event == TRAVEL_EVT_RELEASE)) {
            self->page = parent_of(self->page);
            self->menu_index = 0;
            return true;
        }
        return false;
    case TRAVEL_P05_DETAIL:
        if (button == 0 && (event == TRAVEL_EVT_CLICK ||
                            event == TRAVEL_EVT_RELEASE)) {
            self->page = TRAVEL_P00_HOME;
            self->menu_index = 0;
            return true;
        }
        if (button == 2 && (event == TRAVEL_EVT_HOLD_START ||
                            event == TRAVEL_EVT_RELEASE)) {
            self->page = TRAVEL_P04_MENU;
            self->menu_index = 0;
            return true;
        }
        if (button == 2 && event == TRAVEL_EVT_CLICK) {
            /* complete toggling handled by the app worker; state flip only */
            self->stop_completed = !self->stop_completed;
            return true;
        }
        return false;
    default:
        return false;
    }
}
