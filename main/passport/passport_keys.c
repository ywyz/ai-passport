#include "passport_keys.h"

static uint32_t repeat_period(uint32_t held_ms)
{
    if (held_ms >= PASSPORT_REPEAT_T3_MS)
        return PASSPORT_REPEAT_T3_PERIOD_MS;
    if (held_ms >= PASSPORT_REPEAT_T2_MS)
        return PASSPORT_REPEAT_T2_PERIOD_MS;
    if (held_ms >= PASSPORT_REPEAT_T1_MS)
        return PASSPORT_REPEAT_T1_PERIOD_MS;
    return 0;
}

char passport_key_feed(struct passport_key_state *s, int button, bool down,
                       uint32_t tick_ms)
{
    char out = PASSPORT_EVT_NONE;
    (void)button;
    if (down && !s->pressed) {
        s->pressed = true;
        s->press_tick = tick_ms;
        s->holding = false;
        s->last_repeat_tick = tick_ms;
        out = PASSPORT_EVT_PRESS;
        return out;
    }
    if (!down && s->pressed) {
        s->pressed = false;
        s->last_repeat_tick = tick_ms;
        if (s->holding || (uint32_t)(tick_ms - s->press_tick) >=
                                  PASSPORT_HOLD_START_MS) {
            /* release after hold: never also a click (A01 suppression) */
            s->holding = false;
            out = PASSPORT_EVT_RELEASE;
        } else {
            out = PASSPORT_EVT_CLICK;
        }
        return out;
    }
    if (down && s->pressed) {
        uint32_t held = tick_ms - s->press_tick;
        if (!s->holding && held >= PASSPORT_HOLD_START_MS) {
            s->holding = true;
            s->last_repeat_tick = tick_ms;
            out = PASSPORT_EVT_HOLD_START;
            return out;
        }
        if (s->holding) {
            uint32_t period = repeat_period(held);
            if (period != 0 &&
                    (tick_ms - s->last_repeat_tick) >= period) {
                s->last_repeat_tick = tick_ms;
                out = PASSPORT_EVT_REPEAT;
            }
        }
    }
    return out;
}
