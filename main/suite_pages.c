// main/suite_pages.c —— 状态页与共享 UI 元件。
// 全新界面:深色底、圆角信息卡,不复用基线 ui_pixel 演示外壳。
#include "suite_pages.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_storage.h"
#include "suite_weather.h"
#include "suite_weather_logic.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "suite_pages";

// ---- 主题 ----
#define C_BG 0x0E1420      // 深底
#define C_CARD 0x1A2332    // 卡片
#define C_INK 0xE8EEF7     // 主文字
#define C_DIM 0x8CA0B8     // 次文字
#define C_OK 0x59C98B      // 正常/在线
#define C_WARN 0xE0B454    // 警示/过期
#define C_ERR 0xE06C6C     // 失败

static lv_obj_t *s_scr;
static lv_obj_t *s_wifi_val;
static lv_obj_t *s_time_val;
static lv_obj_t *s_wx_val;
static lv_obj_t *s_battery;
static lv_timer_t *s_timer;

static const char *conn_text(suite_conn_state_t st)
{
    switch (st) {
    case SUITE_CONN_ONLINE: return "Online";
    case SUITE_CONN_CONNECTING: return "Connecting";
    case SUITE_CONN_WAIT_IP: return "Wait IP";
    case SUITE_CONN_RETRY_WAIT: return "Retry";
    default: return "Off";
    }
}

static uint32_t conn_color(suite_conn_state_t st)
{
    switch (st) {
    case SUITE_CONN_ONLINE: return C_OK;
    case SUITE_CONN_RETRY_WAIT: return C_WARN;
    default: return C_DIM;
    }
}

static const char *wx_text(const suite_weather_data_t *d, char *buf, size_t cap)
{
    if (!d->valid) return "Waiting";
    int n = snprintf(buf, cap, "%.1fC %s", d->temperature,
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_CLEAR ? "Clear" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_PARTLY ? "Partly" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_CLOUDY ? "Cloudy" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_FOG ? "Fog" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_DRIZZLE ? "Drizzle" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_RAIN ? "Rain" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_SNOW ? "Snow" :
                     suite_wx_code_group((int)d->wmo_code) == SUITE_WX_STORM ? "Storm" : "-");
    if (n < 0 || (size_t)n >= cap) return "?";
    return buf;
}

// 每帧刷新(250 ms):读状态快照,持锁更新标签。
static void status_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!bsp_lvgl_lock(200)) return;

    suite_conn_state_t st = suite_connection_state();
    lv_label_set_text(s_wifi_val, conn_text(st));
    lv_obj_set_style_text_color(s_wifi_val, lv_color_hex(conn_color(st)), 0);

    char buf[48];
    const suite_time_tracker_t *t = suite_connection_time();
    if (suite_time_is_trusted(t, suite_connection_now())) {
        suite_civil_t c;
        suite_civil_from_epoch(suite_connection_now(), 8 * 3600, &c);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d OK",
                 c.year, c.month, c.day, c.hour, c.minute);
        lv_label_set_text(s_time_val, buf);
        lv_obj_set_style_text_color(s_time_val, lv_color_hex(C_OK), 0);
    } else {
        lv_label_set_text(s_time_val,
                          t->state == SUITE_TIME_UNSYNCED ? "Unsynced" : "Syncing");
        lv_obj_set_style_text_color(s_time_val, lv_color_hex(C_WARN), 0);
    }

    suite_weather_data_t d;
    suite_weather_get(&d);
    char wxbuf[32];
    lv_label_set_text(s_wx_val, wx_text(&d, wxbuf, sizeof(wxbuf)));
    lv_obj_set_style_text_color(s_wx_val,
                                lv_color_hex(d.stale ? C_WARN : C_INK), 0);

    suite_ui_battery_update(s_battery);
    bsp_lvgl_unlock();
}

static void on_status_action(suite_action_t act)
{
    if (act == SUITE_ACTION_OK) {
        // OK:手动重连 + 天气立即刷新(在线且已校时时)。
        suite_connection_reconnect_now();
        (void)suite_weather_request_refresh();
    }
    // UP/DOWN/BACK 状态页不使用。
}

lv_obj_t *suite_ui_battery_create(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(C_DIM), 0);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -12, 10);
    suite_ui_battery_update(label);
    return label;
}

void suite_ui_battery_update(lv_obj_t *label)
{
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(label, "BAT --");
    } else {
        lv_label_set_text_fmt(label, "BAT %d%%", soc);
    }
}

static lv_obj_t *info_row(lv_obj_t *parent, const char *title, int y,
                          lv_obj_t **value)
{
    lv_obj_t *title_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 16, y);
    lv_label_set_text(title_lbl, title);

    lv_obj_t *val = lv_label_create(parent);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(C_INK), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -16, y);
    *value = val;
    return val;
}

static void status_enter(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);

    // 标题
    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_INK), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);
    lv_label_set_text(title, "AI PASSPORT");

    s_battery = suite_ui_battery_create(s_scr);

    // 信息卡
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_set_size(card, 208, 200);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    info_row(card, "Wi-Fi", 14, &s_wifi_val);
    info_row(card, "Time", 60, &s_time_val);
    info_row(card, "Weather", 106, &s_wx_val);

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    lv_label_set_text(hint, "OK: refresh");

    s_timer = lv_timer_create(status_tick, 250, NULL);
    lv_screen_load(s_scr);
}

static void status_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

const suite_page_t SUITE_PAGE_STATUS = {
    .name = "status",
    .enter = status_enter,
    .exit = status_exit,
    .action = on_status_action,
};
