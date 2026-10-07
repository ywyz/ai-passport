// main/suite_input.c —— 输入 reducer 实现(纯逻辑)。
#include "suite_input.h"

void suite_input_reducer_init(suite_input_reducer_t *r)
{
    r->ok_down = false;
    r->ok_long_sent = false;
    r->ok_double_pending = false;
}

suite_action_t suite_input_reduce(suite_input_reducer_t *r,
                                  suite_btn_t btn, suite_key_ev_t ev)
{
    if (btn == SUITE_BTN_OK) {
        switch (ev) {
        case SUITE_KEY_PRESS:
            r->ok_down = true;
            r->ok_long_sent = false;
            r->ok_double_pending = false;
            return SUITE_ACTION_NONE;
        case SUITE_KEY_LONG:
            // 迟到的 LONG(已松开或已交付)不重复返回。
            if (!r->ok_down || r->ok_long_sent) return SUITE_ACTION_NONE;
            r->ok_long_sent = true;
            return SUITE_ACTION_BACK;
        case SUITE_KEY_CLICK:
            // 长按后松开产生的 CLICK 抑制,避免返回+确认连发。
            r->ok_down = false;
            if (r->ok_long_sent) {
                r->ok_long_sent = false;
                return SUITE_ACTION_NONE;
            }
            // 双击后的残余 CLICK 属于双击手势,吞掉。
            if (r->ok_double_pending) {
                r->ok_double_pending = false;
                return SUITE_ACTION_NONE;
            }
            return SUITE_ACTION_OK;
        case SUITE_KEY_DOUBLE:
            r->ok_down = false;
            r->ok_long_sent = false;
            r->ok_double_pending = true;
            return SUITE_ACTION_NONE;
        default:
            return SUITE_ACTION_NONE;
        }
    }

    // UP/DOWN:仅单击生效;长按/双击/按下瞬间不产生重复移动。
    if (ev == SUITE_KEY_CLICK) {
        return btn == SUITE_BTN_UP ? SUITE_ACTION_UP : SUITE_ACTION_DOWN;
    }
    return SUITE_ACTION_NONE;
}
