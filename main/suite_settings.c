// main/suite_settings.c —— 设置页:选择城市(省市两级选择器入口)与
// 连接状态入口。城市坐标由 suite_city_data.c 提供,NVS 保存与天气刷新
// 在城市页确认时完成。
#include "suite_settings.h"

#include "suite_app.h"
#include "suite_city_picker.h"
#include "suite_pages.h"
#include "suite_provision.h"
#include "suite_storage.h"

#include "bsp_display.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <string.h>

// ---- 主题(与 suite_pages.c 保持一致) ----
#define C_BG 0x0E1420
#define C_CARD 0x1A2332
#define C_SEL 0x24344A
#define C_INK 0xE8EEF7
#define C_DIM 0x8CA0B8

#define ROW_W 216
#define ROW_H 26
#define ROW_STEP 29
#define ROW_Y0 36

#define ROW_CITY 0  // 选择城市(右侧显示当前生效城市)
#define ROW_CONN 1  // 连接状态入口
#define ROW_PROV 2  // 手机扫码 BLE 配网入口(本需求新增;行右侧无值)
#define ROW_TOTAL 3

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_TOTAL];
static lv_obj_t *s_cur_city;
static int s_sel;

static void row_refresh(void)
{
    for (int i = 0; i < ROW_TOTAL; i++) {
        bool sel = i == s_sel;
        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex(sel ? C_SEL : C_CARD), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_rows[i], 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? C_INK : C_DIM), 0);
    }
}

static lv_obj_t *row_create(lv_obj_t *parent, const char *text, int i)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ROW_Y0 + i * ROW_STEP);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &suite_font_16, 0);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);
    return row;
}

static void settings_enter(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &suite_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text(title, "设置");

    (void)suite_ui_battery_create(s_scr);

    s_rows[ROW_CITY] = row_create(s_scr, "选择城市", ROW_CITY);
    // 行右侧展示当前生效城市,读不到时留空。
    char name[32];
    name[0] = '\0';
    suite_city_t cur;
    if (suite_storage_city_get(&cur) == ESP_OK) {
        strlcpy(name, cur.name, sizeof(name));
    }
    s_cur_city = lv_label_create(s_rows[ROW_CITY]);
    lv_obj_set_style_text_font(s_cur_city, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_cur_city, lv_color_hex(C_DIM), 0);
    lv_label_set_text(s_cur_city, name);
    lv_obj_align(s_cur_city, LV_ALIGN_RIGHT_MID, -14, 0);

    s_rows[ROW_CONN] = row_create(s_scr, "连接状态", ROW_CONN);
    s_rows[ROW_PROV] = row_create(s_scr, "手机配网", ROW_PROV);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, ROW_Y0 + ROW_TOTAL * ROW_STEP + 6);
    lv_label_set_text(hint, "确认 长按 返回");

    s_sel = ROW_CITY;
    row_refresh();
    lv_screen_load(s_scr);
}

static void settings_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static void on_settings_action(suite_action_t act)
{
    switch (act) {
    case SUITE_ACTION_UP:
        if (s_sel > ROW_CITY) {
            s_sel--;
            if (bsp_lvgl_lock(200)) {
                row_refresh();
                bsp_lvgl_unlock();
            }
        }
        break;
    case SUITE_ACTION_DOWN:
        if (s_sel < ROW_PROV) {
            s_sel++;
            if (bsp_lvgl_lock(200)) {
                row_refresh();
                bsp_lvgl_unlock();
            }
        }
        break;
    case SUITE_ACTION_OK:
        if (s_sel == ROW_CONN) {
            (void)suite_app_push(&SUITE_PAGE_STATUS);
        } else if (s_sel == ROW_PROV) {
            (void)suite_app_push(&SUITE_PAGE_PROVISION);
        } else {
            (void)suite_app_push(&SUITE_PAGE_PROVINCE);
        }
        break;
    default:
        break;
    }
}

const suite_page_t SUITE_PAGE_SETTINGS = {
    .name = "settings",
    .enter = settings_enter,
    .exit = settings_exit,
    .action = on_settings_action,
};
