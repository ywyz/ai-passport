// main/suite_home.c —— 首页:霓虹三体星轨 + 时钟 + 日期 + 天气行。
// 视觉参考"三体星轨"效果图:纯黑底、发光太阳(双层光晕+白热核心)、
// 离散粒子拖尾、彩色星点、居中标题。场景数学在 suite_scene.c(纯逻辑,
// 10 FPS 上限);本文件只做渲染与快照刷新。时间不可信时时钟显示 "--:--"
// 并给出状态提示,不伪装可用(计划第 7 节)。
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

// ---- 主题 ----
#define C_BG 0x000105    // 纯黑夜空
#define C_INK 0xEAF4FF   // 主文字(冷白)
#define C_DIM 0x7E8CA6   // 次文字
#define C_WARN 0xE0B454  // 警示
#define C_TITLE 0xE6EDF8 // 标题

#define SCENE_CX 120       // 场景中心(屏宽 240)
#define SCENE_CY 128       // 包含光晕的运动区位于副题与时钟之间
#define SCENE_SCALE 88.0f  // 归一化坐标 → 像素比例

#define STAR_COUNT 22
#define TRAIL_SLOTS 8         // 约 0.8 秒短拖尾,快速运动时不铺满整个天区

// 白热化核心色(太阳色向白混合);光晕与拖尾用太阳本色。
static const uint32_t k_core_colors[SUITE_SCENE_SUNS] = {
    0xFFAD9E,  // 焰橙核心
    0x9AF0FF,  // 电青核心
    0xE4AAFF,  // 幻紫核心
};
static const uint32_t k_star_colors[] = {
    0x8A7BD8,  // 紫
    0x5AD8E8,  // 青
    0xFF8A66,  // 橙
    0xC9D6EE,  // 冷白
};

typedef struct {
    float x, y;
} trail_pt_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_glow_in[SUITE_SCENE_SUNS];
static lv_obj_t *s_glow_out[SUITE_SCENE_SUNS];
static lv_obj_t *s_suns[SUITE_SCENE_SUNS];
static lv_obj_t *s_trail[SUITE_SCENE_SUNS][TRAIL_SLOTS];
static trail_pt_t s_hist[SUITE_SCENE_SUNS][TRAIL_SLOTS];  // 环形历史缓冲
static int s_head;
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

// 通用圆点(星点/拖尾/光晕/太阳共用)。
static lv_obj_t *dot_create(lv_obj_t *parent, lv_coord_t size, lv_coord_t cx,
                            lv_coord_t cy, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_pos(dot, cx - size / 2, cy - size / 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, opa, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

// 星空:固定种子 LCG 生成确定性点位/尺寸/配色,铺满天区。
static void stars_create(lv_obj_t *parent)
{
    uint32_t s = 20261007u;
    for (int i = 0; i < STAR_COUNT; i++) {
        s = s * 1664525u + 1013904223u;
        lv_coord_t x = (lv_coord_t)((s >> 16) % 232) + 4;
        lv_coord_t y = (lv_coord_t)(((s >> 8) % 150) + 6);
        lv_coord_t size = (lv_coord_t)((s >> 20) % 2) + 1;
        uint32_t color = k_star_colors[(s >> 12) % 4];
        lv_opa_t opa = (s >> 4) % 2 ? LV_OPA_60 : LV_OPA_40;
        (void)dot_create(parent, size, x, y, color, opa);
    }
}

// 拖尾槽位外观固定:新→旧尺寸 6→1 px、透明度 70%→10%,渲染时只移位置。
static lv_coord_t trail_size(int k)
{
    lv_coord_t sz = (lv_coord_t)(6 - (5 * k) / (TRAIL_SLOTS - 1));
    return sz < 1 ? 1 : sz;
}

static lv_opa_t trail_opa(int k)
{
    return (lv_opa_t)(LV_OPA_70 -
                      (LV_OPA_70 - LV_OPA_10) * k / (TRAIL_SLOTS - 1));
}

static void trails_create(lv_obj_t *parent)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        for (int k = 0; k < TRAIL_SLOTS; k++) {
            s_trail[i][k] = dot_create(parent, trail_size(k), SCENE_CX,
                                       SCENE_CY, s_scene.suns[i].color,
                                       trail_opa(k));
        }
    }
}

static void suns_create(lv_obj_t *parent)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        uint32_t color = s_scene.suns[i].color;
        // 光晕先创建,压在核心下层;尺寸随核心半径每帧更新。
        s_glow_out[i] = dot_create(parent, 24, SCENE_CX, SCENE_CY, color,
                                   LV_OPA_30);
        s_glow_in[i] = dot_create(parent, 16, SCENE_CX, SCENE_CY, color,
                                  LV_OPA_60);
        s_suns[i] = dot_create(parent, 10, SCENE_CX, SCENE_CY,
                               k_core_colors[i], LV_OPA_COVER);
    }
}

static void trails_reset(void)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        for (int k = 0; k < TRAIL_SLOTS; k++) {
            s_hist[i][k].x = s_scene.suns[i].x;
            s_hist[i][k].y = s_scene.suns[i].y;
        }
    }
    s_head = 0;
}

// 每显示帧记录位置;异常重置时清空旧轨迹,避免跨屏连线。
static void trails_record(void)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        s_hist[i][s_head].x = s_scene.suns[i].x;
        s_hist[i][s_head].y = s_scene.suns[i].y;
    }
    s_head = (s_head + 1) % TRAIL_SLOTS;
}

// 帧渲染:核心 + 双层光晕 + 拖尾,只改几何不改样式。
static void scene_render(void)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const suite_scene_sun_t *sun = &s_scene.suns[i];
        float r = sun->radius * SCENE_SCALE;
        if (r < 3.0f) r = 3.0f;
        lv_coord_t cx = (lv_coord_t)(SCENE_CX + sun->x * SCENE_SCALE);
        lv_coord_t cy = (lv_coord_t)(SCENE_CY + sun->y * SCENE_SCALE);
        lv_coord_t core = (lv_coord_t)(2.0f * r);
        lv_coord_t in = (lv_coord_t)(3.2f * r);
        lv_coord_t out = (lv_coord_t)(4.8f * r);
        lv_obj_set_size(s_glow_out[i], out, out);
        lv_obj_set_pos(s_glow_out[i], cx - out / 2, cy - out / 2);
        lv_obj_set_size(s_glow_in[i], in, in);
        lv_obj_set_pos(s_glow_in[i], cx - in / 2, cy - in / 2);
        lv_obj_set_size(s_suns[i], core, core);
        lv_obj_set_pos(s_suns[i], cx - core / 2, cy - core / 2);
        for (int k = 0; k < TRAIL_SLOTS; k++) {
            // k=0 最新;环形缓冲从 head-1 往回走。
            int idx = (s_head - 1 - k + TRAIL_SLOTS) % TRAIL_SLOTS;
            lv_coord_t tx =
                (lv_coord_t)(SCENE_CX + s_hist[i][idx].x * SCENE_SCALE);
            lv_coord_t ty =
                (lv_coord_t)(SCENE_CY + s_hist[i][idx].y * SCENE_SCALE);
            lv_coord_t sz = trail_size(k);
            lv_obj_set_pos(s_trail[i][k], tx - sz / 2, ty - sz / 2);
        }
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
        // "日一二三四五六" 是 UTF-8 定长 3 字节/字且无结尾符的字面量,
        // 必须按 3 字节拷贝;直接 %s 会把后续字符全部带出("星期三四五六")。
        char wk[4];
        memcpy(wk, "日一二三四五六" + c.weekday * 3, 3);
        wk[3] = '\0';
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d 星期%s", c.year, c.month,
                 c.day, wk);
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
    uint32_t resets = s_scene.reset_count;
    suite_scene_advance(&s_scene);
    if (s_scene.reset_count != resets) trails_reset();
    trails_record();
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

    suite_scene_init(&s_scene);
    trails_reset();
    trails_create(s_scr);
    suns_create(s_scr);

    // 居中标题 + 宽字距英文副题(参考图版式)。
    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &suite_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(title, "三体星轨");

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &suite_font_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(sub, "THREE-BODY ORBITS");

    s_battery = suite_ui_battery_create(s_scr);

    // 时钟区(内容由首个 tick 填充,避免双份排版逻辑)
    s_clock_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_clock_lbl, &suite_font_clock, 0);
    lv_obj_set_style_text_color(s_clock_lbl, lv_color_hex(C_INK), 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_MID, 0, 200);
    lv_label_set_text(s_clock_lbl, "--:--");

    s_date_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_date_lbl, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_date_lbl, LV_ALIGN_TOP_MID, 0, 240);
    lv_label_set_text(s_date_lbl, "");

    s_wx_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_wx_lbl, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_wx_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_wx_lbl, LV_ALIGN_TOP_MID, 0, 266);
    lv_label_set_text(s_wx_lbl, "");

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
