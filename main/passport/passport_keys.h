/**
 * passport_keys.h - pure button event state machine (D1 travel UI).
 *
 * Events PRESS / RELEASE / CLICK / HOLD_START / REPEAT per PRD section 4:
 * - long OK starts at 700 ms, release after a hold never emits CLICK;
 * - UP/DOWN move once on press, then repeat at 450 ms -> 180 ms, after
 *   1.5 s -> 90 ms, after 3 s -> 60 ms;
 * - millisecond tick input is monotonic; no wall clock involved.
 * All timings are compile-time constants here so host tests can assert the
 * accepted schedule; on-device verification happens in D1c.
 */
#ifndef PASSPORT_KEYS_H
#define PASSPORT_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#define PASSPORT_BTN_UP 0
#define PASSPORT_BTN_DOWN 1
#define PASSPORT_BTN_OK 2
#define PASSPORT_BTN_COUNT 3

#define PASSPORT_HOLD_START_MS 700
#define PASSPORT_REPEAT_T1_MS 450
#define PASSPORT_REPEAT_T1_PERIOD_MS 180
#define PASSPORT_REPEAT_T2_MS 1500
#define PASSPORT_REPEAT_T2_PERIOD_MS 90
#define PASSPORT_REPEAT_T3_MS 3000
#define PASSPORT_REPEAT_T3_PERIOD_MS 60

enum passport_key_event {
    PASSPORT_EVT_NONE = 0,
    PASSPORT_EVT_PRESS,
    PASSPORT_EVT_RELEASE,
    PASSPORT_EVT_CLICK,     /* short press */
    PASSPORT_EVT_HOLD_START,
    PASSPORT_EVT_REPEAT,
};

struct passport_key_state {
    bool pressed;
    uint32_t press_tick;
    bool holding;
    uint32_t last_repeat_tick;
};

/* One monotonic tick per button state; multiple of 1 ms typical. */
char passport_key_feed(struct passport_key_state *s, int button, bool down,
                       uint32_t tick_ms);

#endif /* PASSPORT_KEYS_H */
