// main/suite_city_picker.c —— 省市两级城市选择页实现。
// 省份页 34 行、城市页至多 21 行(广东),行对象随页建/删,页内常驻对象
// 数量远低于 LVGL 池预算。输入回调运行在输入任务上下文:改行样式前持
// bsp_lvgl_lock,解锁后再经 suite_app 导航(导航内部自取锁)。
#include "suite_city_picker.h"

#include "suite_app.h"
#include "suite_city_data.h"
#include "suite_pages.h"
#include "suite_storage.h"
#include "suite_weather.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <string.h>

static const char *TAG = "suite_city_picker";

// ---- 主题(与 suite_pages.c 保持一致) ----
#define C_BG 0x0E1420
#define C_CARD 0x1A2332
#define C_SEL 0x24344A
#define C_INK 0xE8EEF7
#define C_DIM 0x8CA0B8

// 行几何与设置页一致:行高 26,间距 29,首行 y=36。
#define ROW_W 216
#define ROW_H 26
#define ROW_STEP 29
#define ROW_Y0 36

// 城市页行数上限:省级行政区中地级市最多为广东 21 个,
// tests/test_suite_city_data.c 保证该上界,数组无需动态分配。
#define CITY_ROWS_MAX 21

static int s_prov_sel;   // 省份页当前选中行
static int s_city_prov;  // 城市页当前展示的省索引
static int s_city_sel;   // 城市页当前选中行

// ---- 共享行绘制 ----

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

static void rows_refresh(lv_obj_t *const *rows, int count, int sel)
{
    for (int i = 0; i < count; i++) {
        bool on = i == sel;
        lv_obj_set_style_bg_color(rows[i], lv_color_hex(on ? C_SEL : C_CARD), 0);
        lv_obj_t *lbl = lv_obj_get_child(rows[i], 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(on ? C_INK : C_DIM), 0);
    }
    // 列表超出一屏:把选中行滚入可视区。
    lv_obj_scroll_to_view(rows[sel], LV_ANIM_OFF);
}

// 输入任务上下文中改 LVGL 样式前必须持锁;取锁失败则跳过本次重绘,
// 选中状态已更新,下一次按键会补上。
static void refresh_locked(lv_obj_t *const *rows, int count, int sel)
{
    if (bsp_lvgl_lock(200)) {
        rows_refresh(rows, count, sel);
        bsp_lvgl_unlock();
    }
}

static lv_obj_t *list_screen_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    lv_obj_t *title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(title_lbl, &suite_font_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text(title_lbl, title);

    (void)suite_ui_battery_create(scr);
    return scr;
}

static void list_hint_create(lv_obj_t *scr, int row_count)
{
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    // 提示随内容排在末行之后,不遮挡列表。
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, ROW_Y0 + row_count * ROW_STEP + 6);
    lv_label_set_text(hint, "确认 长按 返回");
}

// 读取当前生效城市名;读不到为空串。
static void current_city_name(char *out, size_t cap)
{
    out[0] = '\0';
    suite_city_t cur;
    if (suite_storage_city_get(&cur) == ESP_OK) {
        strlcpy(out, cur.name, cap);
    }
}

// ---- 省份页 ----

static lv_obj_t *s_p_scr;
static lv_obj_t *s_p_rows[34];  // 34 个省级行政区,由主机测试保证

static void province_enter(void)
{
    const int count = suite_city_province_count();
    s_p_scr = list_screen_create("选择省份");
    for (int i = 0; i < count; i++) {
        s_p_rows[i] = row_create(s_p_scr, suite_city_province_at(i)->name, i);
    }
    list_hint_create(s_p_scr, count);

    // 默认选中当前城市所在省;无城市则选中第一省。
    char name[32];
    current_city_name(name, sizeof(name));
    int p, c;
    s_prov_sel = suite_city_find(name, &p, &c) ? p : 0;

    rows_refresh(s_p_rows, count, s_prov_sel);
    lv_screen_load(s_p_scr);
}

static void province_exit(void)
{
    if (s_p_scr) {
        lv_obj_delete(s_p_scr);
        s_p_scr = NULL;
    }
}

static void province_action(suite_action_t act)
{
    const int count = suite_city_province_count();
    switch (act) {
    case SUITE_ACTION_UP:
        if (s_prov_sel > 0) {
            s_prov_sel--;
            refresh_locked(s_p_rows, count, s_prov_sel);
        }
        break;
    case SUITE_ACTION_DOWN:
        if (s_prov_sel < count - 1) {
            s_prov_sel++;
            refresh_locked(s_p_rows, count, s_prov_sel);
        }
        break;
    case SUITE_ACTION_OK:
        s_city_prov = s_prov_sel;
        (void)suite_app_push(&SUITE_PAGE_CITY);
        break;
    default:
        break;
    }
}

const suite_page_t SUITE_PAGE_PROVINCE = {
    .name = "province",
    .enter = province_enter,
    .exit = province_exit,
    .action = province_action,
};

// ---- 城市页 ----

static lv_obj_t *s_c_scr;
static lv_obj_t *s_c_rows[CITY_ROWS_MAX];

static void city_enter(void)
{
    const suite_province_def_t *prov = suite_city_province_at(s_city_prov);
    s_c_scr = list_screen_create(prov->name);
    for (int i = 0; i < prov->city_count; i++) {
        s_c_rows[i] = row_create(s_c_scr, prov->cities[i].name, i);
    }
    list_hint_create(s_c_scr, prov->city_count);

    // 默认选中当前生效城市(若属于该省),否则选中第一市。
    char name[32];
    current_city_name(name, sizeof(name));
    s_city_sel = 0;
    for (int i = 0; i < prov->city_count; i++) {
        if (strcmp(prov->cities[i].name, name) == 0) {
            s_city_sel = i;
            break;
        }
    }

    rows_refresh(s_c_rows, prov->city_count, s_city_sel);
    lv_screen_load(s_c_scr);
}

static void city_exit(void)
{
    if (s_c_scr) {
        lv_obj_delete(s_c_scr);
        s_c_scr = NULL;
    }
}

static void city_action(suite_action_t act)
{
    const suite_province_def_t *prov = suite_city_province_at(s_city_prov);
    switch (act) {
    case SUITE_ACTION_UP:
        if (s_city_sel > 0) {
            s_city_sel--;
            refresh_locked(s_c_rows, prov->city_count, s_city_sel);
        }
        break;
    case SUITE_ACTION_DOWN:
        if (s_city_sel < prov->city_count - 1) {
            s_city_sel++;
            refresh_locked(s_c_rows, prov->city_count, s_city_sel);
        }
        break;
    case SUITE_ACTION_OK: {
        const suite_city_def_t *def = &prov->cities[s_city_sel];
        suite_city_t city;
        memset(&city, 0, sizeof(city));
        strlcpy(city.name, def->name, sizeof(city.name));
        city.lat_x1e4 = def->lat_x1e4;
        city.lon_x1e4 = def->lon_x1e4;
        city.tz_off_s = 8 * 3600;
        esp_err_t err = suite_storage_city_set(&city);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "城市保存失败: %s", esp_err_to_name(err));
        }
        // 在线且时间可信时立即按新城市刷新;否则 worker 上线后自取。
        (void)suite_weather_request_refresh();
        // 依次退出城市页与省份页,回到设置页。
        (void)suite_app_back();
        (void)suite_app_back();
        break;
    }
    default:
        break;
    }
}

const suite_page_t SUITE_PAGE_CITY = {
    .name = "city",
    .enter = city_enter,
    .exit = city_exit,
    .action = city_action,
};
