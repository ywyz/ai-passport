/**
 * travel_ui.h - the D1 travel application LVGL screens (public seam).
 *
 * Screens are created/updated only from the LVGL task (or under
 * bsp_lvgl_lock()). Host tests exercise travel_state; the LVGL tree itself
 * requires the firmware build/device rendering.
 */
#ifndef TRAVEL_UI_H
#define TRAVEL_UI_H

#include "travel_state.h"
#include <stdbool.h>

struct travel_ui {
    /* opaque LVGL tree internally */
    void *ctx;
};

/* build pages once; pages are shown/hidden by id */
int travel_ui_init(struct travel_ui *ui, struct travel_state *state);

/* refresh visible page after a state change (both under LVGL lock) */
void travel_ui_render(struct travel_ui *ui);

/* tear down every LVGL object created by pages (no tasks/timers kept) */
void travel_ui_deinit(struct travel_ui *ui);

#endif /* TRAVEL_UI_H */
