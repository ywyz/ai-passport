// tests/test_suite_input.c —— 输入 reducer 主机测试。
#include <assert.h>

#include "suite_input.h"

int main(void) {
    suite_input_reducer_t r;
    suite_input_reducer_init(&r);

    // UP/DOWN 单击移动;长按不重复。
    assert(suite_input_reduce(&r, SUITE_BTN_UP, SUITE_KEY_CLICK) == SUITE_ACTION_UP);
    assert(suite_input_reduce(&r, SUITE_BTN_DOWN, SUITE_KEY_CLICK) == SUITE_ACTION_DOWN);
    assert(suite_input_reduce(&r, SUITE_BTN_UP, SUITE_KEY_PRESS) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_UP, SUITE_KEY_LONG) == SUITE_ACTION_NONE);

    // OK 单击确认。
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_PRESS) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_CLICK) == SUITE_ACTION_OK);

    // OK 长按返回,且长按后的 CLICK 被抑制(不产生确认)。
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_PRESS) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_LONG) == SUITE_ACTION_BACK);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_CLICK) == SUITE_ACTION_NONE);

    // 迟到 LONG(无按下上下文)丢弃,防止退页后残留长按触发返回;
    // 裸 CLICK 与"PRESS 被丢队列"无法区分,按有效确认处理。
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_LONG) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_CLICK) == SUITE_ACTION_OK);

    // 双击不产生动作,且重置按下状态。
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_PRESS) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_DOUBLE) == SUITE_ACTION_NONE);
    assert(suite_input_reduce(&r, SUITE_BTN_OK, SUITE_KEY_CLICK) == SUITE_ACTION_NONE);

    return 0;
}
