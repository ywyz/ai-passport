// main/suite_launcher.c —— 功能启动器(翻译/验证码/票夹/待办/设置)与占位页。
// 未实现项进入占位页,明确告知"后续阶段接入",不伪装可用。
#include "suite_launcher.h"

#include "suite_pages.h"
#include "suite_settings.h"
#include "suite_tasks.h"
#include "suite_translation.h"

#include "bsp_display.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <string.h>

// ---- 主题(与 suite_pages.c 保持一致) ----
#define C_BG 0x0E1420
#define C_CARD 0x1A2332
#define C_SEL 0x24344A     // 选中行
#define C_INK 0xE8EEF7
#define C_DIM 0x8CA0B8

#define LAUNCH_COUNT 5

static const char *k_items[LAUNCH_COUNT] = {
    "翻译", "验证码", "票夹", "待办", "设置",
};

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[LAUNCH_COUNT];
static lv_obj_t *s_battery;
static int s_sel;

static void row_refresh(void)
{
    for (int i = 0; i < LAUNCH_COUNT; i++) {
        bool sel = i == s_sel;
        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex(sel ? C_SEL : C_CARD), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_rows[i], 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? C_INK : C_DIM), 0);
    }
}

static void launcher_enter(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &suite_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text(title, "功能");

    s_battery = suite_ui_battery_create(s_scr);

    for (int i = 0; i < LAUNCH_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_set_size(row, 216, 42);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 40 + i * 48);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &suite_font_16, 0);
        lv_label_set_text(lbl, k_items[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);
        s_rows[i] = row;
    }

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(hint, "长按 返回");

    s_sel = 0;
    row_refresh();
    lv_screen_load(s_scr);
}

static void launcher_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static void on_launcher_action(suite_action_t act)
{
    switch (act) {
    case SUITE_ACTION_UP:
        if (s_sel > 0) {
            s_sel--;
            row_refresh();
        }
        break;
    case SUITE_ACTION_DOWN:
        if (s_sel < LAUNCH_COUNT - 1) {
            s_sel++;
            row_refresh();
        }
        break;
    case SUITE_ACTION_OK:
        if (s_sel == 4) {
            (void)suite_app_push(&SUITE_PAGE_SETTINGS);
        } else if (s_sel == 3) {
            (void)suite_app_push(&SUITE_PAGE_TASKS);
        } else if (s_sel == 0) {
            (void)suite_app_push(&SUITE_PAGE_TRANSLATION);
        } else {
            (void)suite_app_push(&SUITE_PAGE_STUB);
        }
        break;
    default:
        break;
    }
}

const suite_page_t SUITE_PAGE_LAUNCHER = {
    .name = "launcher",
    .enter = launcher_enter,
    .exit = launcher_exit,
    .action = on_launcher_action,
};

// ---- 占位页 ----
static lv_obj_t *s_stub_scr;

static void stub_enter(void)
{
    s_stub_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_stub_scr, lv_color_hex(C_BG), 0);

    lv_obj_t *t = lv_label_create(s_stub_scr);
    lv_obj_set_style_text_font(t, &suite_font_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_INK), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(t, "功能开发中");

    lv_obj_t *s = lv_label_create(s_stub_scr);
    lv_obj_set_style_text_font(s, &suite_font_16, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(C_DIM), 0);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 12);
    lv_label_set_text(s, "后续阶段接入");

    lv_obj_t *h = lv_label_create(s_stub_scr);
    lv_obj_set_style_text_font(h, &suite_font_16, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(C_DIM), 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(h, "长按 返回");

    lv_screen_load(s_stub_scr);
}

static void stub_exit(void)
{
    if (s_stub_scr) {
        lv_obj_delete(s_stub_scr);
        s_stub_scr = NULL;
    }
}

static void on_stub_action(suite_action_t act)
{
    (void)act;  // BACK 由外壳统一处理;占位页无其他交互
}

const suite_page_t SUITE_PAGE_STUB = {
    .name = "stub",
    .enter = stub_enter,
    .exit = stub_exit,
    .action = on_stub_action,
};
