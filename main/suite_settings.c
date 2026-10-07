// main/suite_settings.c —— 设置页:城市选择(保存 NVS + 触发天气刷新)
// 与连接状态入口。城市坐标为市中心近似值,对 Open-Meteo 网格精度足够。
#include "suite_settings.h"

#include "suite_pages.h"
#include "suite_storage.h"
#include "suite_weather.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <string.h>

static const char *TAG = "suite_settings";

// ---- 主题(与 suite_pages.c 保持一致) ----
#define C_BG 0x0E1420
#define C_CARD 0x1A2332
#define C_SEL 0x24344A
#define C_INK 0xE8EEF7
#define C_DIM 0x8CA0B8

typedef struct {
    const char *name;
    int32_t lat_x1e4;
    int32_t lon_x1e4;
} city_preset_t;

static const city_preset_t k_cities[] = {
    { "北京", 395427, 1163916 },
    { "上海", 312304, 1214737 },
    { "广州", 231291, 1132644 },
    { "深圳", 225431, 1140579 },
    { "杭州", 302741, 1201551 },
    { "成都", 305728, 1040668 },
    { "武汉", 305928, 1143055 },
    { "西安", 343416, 1089398 },
};
#define CITY_COUNT (sizeof(k_cities) / sizeof(k_cities[0]))

#define ROW_CONN ((int)(CITY_COUNT))      // 最后一行:连接状态入口
#define ROW_TOTAL ((int)(CITY_COUNT) + 1)

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_TOTAL];
static lv_obj_t *s_battery;
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
    lv_obj_set_size(row, 216, 26);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 36 + i * 29);
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
    lv_label_set_text(title, "城市设置");

    s_battery = suite_ui_battery_create(s_scr);

    for (int i = 0; i < (int)CITY_COUNT; i++) {
        s_rows[i] = row_create(s_scr, k_cities[i].name, i);
    }
    s_rows[ROW_CONN] = row_create(s_scr, "连接状态", ROW_CONN);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(hint, "确认 长按 返回");

    // 默认选中当前生效城市;读不到则选中第一项。
    suite_city_t cur;
    s_sel = 0;
    if (suite_storage_city_get(&cur) == ESP_OK) {
        for (int i = 0; i < (int)CITY_COUNT; i++) {
            if (strcmp(k_cities[i].name, cur.name) == 0) {
                s_sel = i;
                break;
            }
        }
    }
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
        if (s_sel > 0) {
            s_sel--;
            row_refresh();
        }
        break;
    case SUITE_ACTION_DOWN:
        if (s_sel < ROW_TOTAL - 1) {
            s_sel++;
            row_refresh();
        }
        break;
    case SUITE_ACTION_OK:
        if (s_sel == ROW_CONN) {
            (void)suite_app_push(&SUITE_PAGE_STATUS);
        } else {
            suite_city_t city;
            memset(&city, 0, sizeof(city));
            strlcpy(city.name, k_cities[s_sel].name, sizeof(city.name));
            city.lat_x1e4 = k_cities[s_sel].lat_x1e4;
            city.lon_x1e4 = k_cities[s_sel].lon_x1e4;
            city.tz_off_s = 8 * 3600;
            esp_err_t err = suite_storage_city_set(&city);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "城市保存失败: %s", esp_err_to_name(err));
            }
            // 在线且时间可信时立即按新城市刷新;否则 worker 上线后自取。
            (void)suite_weather_request_refresh();
            (void)suite_app_back();
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
