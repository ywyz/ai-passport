// main/suite_home.c —— 首页:三体场景(3 颗太阳/星空/地平线) + 时钟 + 天气行。
// 场景数学在 suite_scene.c(纯逻辑,10 FPS 上限);本文件只做渲染与快照刷新。
// 时间不可信时时钟显示 "--:--" 并给出状态提示,不伪装可用(计划第 7 节)。
#include "suite_home.h"

#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_launcher.h"
#include "suite_pages.h"
#include "suite_scene.h"
#include "suite_storage.h"
#include "suite_weather.h"
#include "suite_weather_logic.h"

#include "bsp_display.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <stdio.h>
#include <string.h>

// ---- 主题(与 suite_pages.c 保持一致) ----
#define C_BG 0x070B14      // 夜空深底
#define C_INK 0xE8EEF7     // 主文字
#define C_DIM 0x8CA0B8     // 次文字
#define C_WARN 0xE0B454    // 警示
#define C_STAR 0x5A6B85    // 星点
#define C_HORIZON 0x22304A // 地平线

#define SCENE_CX 120       // 场景中心(屏宽 240)
#define SCENE_CY 118
#define SCENE_SCALE 100.0f // 归一化坐标 → 像素比例

#define STAR_COUNT 18

static lv_obj_t *s_scr;
static lv_obj_t *s_suns[SUITE_SCENE_SUNS];
static lv_obj_t *s_clock_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_wx_lbl;
static lv_obj_t *s_battery;
static lv_timer_t *s_timer;
static suite_scene_state_t s_scene;
// 时钟区已渲染状态:-1 未渲染,0 可信,1/2/3 = UNSYNCED/SYNCING/过期
static int s_shown_time_state = -1;
static int s_shown_min = -1;
static char s_last_wx_text[64];
static char s_city_name[sizeof(((suite_city_t *)0)->name)];

static void scene_render(void);

static const char *wx_group_text(suite_wx_group_t g)
{
    switch (g) {
    case SUITE_WX_CLEAR:   return "晴";
    case SUITE_WX_PARTLY:  return "多云";
    case SUITE_WX_CLOUDY:  return "阴";
    case SUITE_WX_FOG:     return "雾";
    case SUITE_WX_DRIZZLE: return "小雨";
    case SUITE_WX_RAIN:    return "雨";
    case SUITE_WX_SNOW:    return "雪";
    case SUITE_WX_STORM:   return "雷雨";
    default:               return "--";
    }
}

// 归一化太阳位置 → 屏幕坐标(半径下限 3 px 防不可见)。
static void scene_render(void)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const suite_scene_sun_t *sun = &s_scene.suns[i];
        lv_coord_t r = (lv_coord_t)(sun->radius * SCENE_SCALE);
        if (r < 3) r = 3;
        lv_obj_set_size(s_suns[i], 2 * r, 2 * r);
        lv_obj_set_pos(s_suns[i],
                       (lv_coord_t)(SCENE_CX + sun->x * SCENE_SCALE) - r,
                       (lv_coord_t)(SCENE_CY + sun->y * SCENE_SCALE) - r);
    }
}

// 星空:固定种子 LCG 生成确定性点位,集中在场景带。
static void stars_create(lv_obj_t *parent)
{
    uint32_t s = 20261007u;
    for (int i = 0; i < STAR_COUNT; i++) {
        s = s * 1664525u + 1013904223u;
        lv_coord_t x = (lv_coord_t)((s >> 16) % 234);
        lv_coord_t y = (lv_coord_t)(((s >> 8) % 178) + 8);
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 2, 2);
        lv_obj_set_pos(dot, x, y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(C_STAR), 0);
        lv_obj_set_style_bg_opa(dot, (s >> 4) % 2 ? LV_OPA_60 : LV_OPA_40, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void suns_create(lv_obj_t *parent)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        lv_obj_t *sun = lv_obj_create(parent);
        lv_obj_set_size(sun, 20, 20);
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sun, lv_color_hex(s_scene.suns[i].color), 0);
        lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sun, 0, 0);
        lv_obj_set_style_pad_all(sun, 0, 0);
        lv_obj_clear_flag(sun, LV_OBJ_FLAG_SCROLLABLE);
        s_suns[i] = sun;
    }
}

// 时钟区:可信→HH:MM+日期;否则 "--:--"+状态文案(警示色)。状态或分钟
// 变化才重绘。
static void clock_refresh(void)
{
    int64_t now = suite_connection_now();
    const suite_time_tracker_t *t = suite_connection_time();
    char buf[48];

    if (suite_time_is_trusted(t, now)) {
        suite_civil_t c;
        suite_civil_from_epoch(now, 8 * 3600, &c);
        if (s_shown_time_state == 0 && c.minute == s_shown_min) return;
        s_shown_time_state = 0;
        s_shown_min = c.minute;
        snprintf(buf, sizeof(buf), "%02d:%02d", c.hour, c.minute);
        lv_label_set_text(s_clock_lbl, buf);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d 星期%s", c.year, c.month,
                 c.day, "日一二三四五六" + c.weekday * 3);
        lv_label_set_text(s_date_lbl, buf);
        lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(C_DIM), 0);
    } else {
        int idx = 1 + (int)t->state;  // UNSYNCED→1 SYNCING→2 SYNCED(过期)→3
        if (s_shown_time_state == idx) return;
        s_shown_time_state = idx;
        lv_label_set_text(s_clock_lbl, "--:--");
        lv_label_set_text(s_date_lbl,
                          t->state == SUITE_TIME_UNSYNCED ? "未校时" :
                          t->state == SUITE_TIME_SYNCING ? "校时中" : "校时过期");
        lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(C_WARN), 0);
    }
}

static void weather_refresh(void)
{
    suite_weather_data_t d;
    suite_weather_get(&d);
    char buf[64];  // 城市名(≤31B) + 温度 + 组名最坏组合仍留余量
    uint32_t color;
    if (!d.valid) {
        snprintf(buf, sizeof(buf), "%s 等待数据", s_city_name);
        color = C_DIM;
    } else {
        bool fresh = !d.stale;
        snprintf(buf, sizeof(buf), "%s %.1f° %s", s_city_name, d.temperature,
                 fresh ? wx_group_text(suite_wx_code_group((int)d.wmo_code))
                       : "数据过期");
        color = fresh ? C_INK : C_WARN;
    }
    if (strcmp(buf, s_last_wx_text) == 0) return;
    strlcpy(s_last_wx_text, buf, sizeof(s_last_wx_text));
    lv_label_set_text(s_wx_lbl, buf);
    lv_obj_set_style_text_color(s_wx_lbl, lv_color_hex(color), 0);
}

// 10 FPS 场景帧;时钟/天气只在变化时重绘。
static void home_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!bsp_lvgl_lock(50)) return;  // 拿不到锁就丢帧,下轮再画
    suite_scene_advance(&s_scene);
    scene_render();
    clock_refresh();
    weather_refresh();
    suite_ui_battery_update(s_battery);
    bsp_lvgl_unlock();
}

static void on_home_action(suite_action_t act)
{
    if (act == SUITE_ACTION_OK) {
        (void)suite_app_push(&SUITE_PAGE_LAUNCHER);
    }
}

static void home_enter(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);

    suite_city_t city;
    if (suite_storage_city_get(&city) == ESP_OK) {
        strlcpy(s_city_name, city.name, sizeof(s_city_name));
    } else {
        strlcpy(s_city_name, "北京", sizeof(s_city_name));
    }
    s_last_wx_text[0] = '\0';

    stars_create(s_scr);

    // 地平线(装饰:落日线)
    lv_obj_t *horizon = lv_obj_create(s_scr);
    lv_obj_set_size(horizon, 240, 2);
    lv_obj_set_pos(horizon, 0, SCENE_CY + (lv_coord_t)(0.88f * SCENE_SCALE));
    lv_obj_set_style_bg_color(horizon, lv_color_hex(C_HORIZON), 0);
    lv_obj_set_style_bg_opa(horizon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(horizon, 0, 0);
    lv_obj_set_style_pad_all(horizon, 0, 0);
    lv_obj_clear_flag(horizon, LV_OBJ_FLAG_SCROLLABLE);

    suite_scene_init(&s_scene);
    suns_create(s_scr);

    // 标题与电池
    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &suite_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text(title, "三体时钟");

    s_battery = suite_ui_battery_create(s_scr);

    // 时钟区(内容由首个 tick 填充,避免双份排版逻辑)
    s_clock_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_clock_lbl, &suite_font_clock, 0);
    lv_obj_set_style_text_color(s_clock_lbl, lv_color_hex(C_INK), 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_MID, 0, 192);
    lv_label_set_text(s_clock_lbl, "--:--");

    s_date_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_date_lbl, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_date_lbl, LV_ALIGN_TOP_MID, 0, 232);
    lv_label_set_text(s_date_lbl, "");

    s_wx_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_wx_lbl, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_wx_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_wx_lbl, LV_ALIGN_TOP_MID, 0, 260);
    lv_label_set_text(s_wx_lbl, "");

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_label_set_text(hint, "确认 > 功能");

    s_shown_time_state = -1;
    s_shown_min = -1;
    s_timer = lv_timer_create(home_tick, SUITE_SCENE_FPS_MS, NULL);
    lv_screen_load(s_scr);
}

static void home_exit(void)
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

const suite_page_t SUITE_PAGE_HOME = {
    .name = "home",
    .enter = home_enter,
    .exit = home_exit,
    .action = on_home_action,
};
