// main/suite_input.h —— 五合一应用输入 reducer(纯逻辑,可主机测试)。
// 把 BSP 按键事件流归约为语义动作:UP/DOWN 移动、OK 确认、OK 长按返回。
// 规则依据 suite-plan 第 7 节:列表上下移动、确认打开;长按确认进入含返回的操作菜单。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 与 bsp_button.h 解耦的按键/事件编码;设备侧 glue 负责映射。
typedef enum {
    SUITE_BTN_UP = 0,
    SUITE_BTN_DOWN,
    SUITE_BTN_OK,
} suite_btn_t;

typedef enum {
    SUITE_KEY_PRESS = 0,
    SUITE_KEY_CLICK,
    SUITE_KEY_DOUBLE,
    SUITE_KEY_LONG,
} suite_key_ev_t;

typedef enum {
    SUITE_ACTION_NONE = 0,
    SUITE_ACTION_UP,
    SUITE_ACTION_DOWN,
    SUITE_ACTION_OK,
    SUITE_ACTION_BACK,
} suite_action_t;

typedef struct {
    bool ok_down;            // OK 当前处于按下状态
    bool ok_long_sent;       // 本次按下已交付过 BACK(长按)
    bool ok_double_pending;  // 刚发生双击,吞掉其后的残余 CLICK
} suite_input_reducer_t;

void suite_input_reducer_init(suite_input_reducer_t *r);

// 输入一个事件,返回语义动作。无状态副作用以外的输出。
// 迟到/乱序事件(未按下就收到 LONG/CLICK)一律丢弃,防止退页后的残留回调
// 触发误操作。
suite_action_t suite_input_reduce(suite_input_reducer_t *r,
                                  suite_btn_t btn, suite_key_ev_t ev);
