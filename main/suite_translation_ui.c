#include "suite_translation.h"

#include "suite_pages.h"
#include "suite_translation_usb.h"

#include "bsp_display.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <stdio.h>
#include <string.h>

extern const lv_font_t suite_tasks_font_16;

#define C_BG 0x0e1420
#define C_CARD 0x1a2332
#define C_SEL 0x24344a
#define C_INK 0xe8eef7
#define C_DIM 0x8ca0b8
#define C_WARN 0xe0b454

#define ROW_COUNT 4
#define USB_WINDOW_SECONDS 60

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_body;
static lv_obj_t *s_source;
static lv_obj_t *s_target;
static lv_obj_t *s_source_caption;
static lv_obj_t *s_target_caption;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_rows[ROW_COUNT];
static lv_obj_t *s_row_labels[ROW_COUNT];
static lv_timer_t *s_timer;
static unsigned s_serial;
static unsigned s_choice;
static bool s_loaded;
static bool s_reset_confirm;
static bool s_config_open;
static int64_t s_config_started_us;
static char s_config_note[64];
static int64_t s_config_note_until_us;

static lv_obj_t *static_label(lv_obj_t *parent, const char *text,
                              uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &suite_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *body_label(lv_obj_t *parent, const char *text,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &suite_tasks_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}


static void set_caption_hidden(lv_obj_t *label, bool hidden)
{
    if (!label) return;
    if (hidden) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

/*
 * The TLS preflight failure uses the existing result labels for two compact
 * numeric lines and hides the source/target captions only while this snapshot
 * is valid. Ordinary results and partial-source failures keep the original
 * captions and body font. All rendering is ASCII already covered by
 * suite_font_16; worst-case values wrap and remain reachable through the
 * existing scrollable body.
 */
static void show_tls_diagnostic(const suite_translation_tls_diag_t *diag)
{
    char text[96];
    int32_t code = diag->tls_code;
    unsigned long magnitude = code < 0
                                  ? (unsigned long)(-(int64_t)code)
                                  : (unsigned long)code;
    lv_obj_set_style_text_font(s_source, &suite_font_16, 0);
    snprintf(text, sizeof(text), "E%lX T%s%lX V%lX\nN%d F%lu L%lu",
             (unsigned long)(uint32_t)diag->esp_err_code,
             code < 0 ? "-" : "", magnitude,
             (unsigned long)diag->verify_flags,
             (int)diag->sys_error,
             (unsigned long)diag->free_bytes,
             (unsigned long)diag->largest_bytes);
    lv_label_set_text(s_source, text);
    set_caption_hidden(s_source_caption, true);
    set_caption_hidden(s_target_caption, true);
    lv_obj_add_flag(s_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
}

static void show_translation_text(const suite_translation_view_t *view)
{
    set_caption_hidden(s_source_caption, false);
    set_caption_hidden(s_target_caption, false);
    lv_obj_remove_flag(s_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_source, &suite_tasks_font_16, 0);
    lv_obj_set_style_text_font(s_target, &suite_tasks_font_16, 0);
    lv_label_set_text(s_source, view->source[0] ? view->source : "无");
    lv_label_set_text(s_target, view->target[0] ? view->target : "无");
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
}

static const char *fallback_status(suite_translation_state_t state)
{
    switch (state) {
    case SUITE_TRANSLATION_WORKING: return "正在处理";
    case SUITE_TRANSLATION_RECORDING: return "正在录音";
    case SUITE_TRANSLATION_DONE: return "翻译完成";
    case SUITE_TRANSLATION_FAILED: return "翻译失败";
    case SUITE_TRANSLATION_CANCELLED: return "已取消任务";
    case SUITE_TRANSLATION_EXPIRED: return "结果已过期，请重新翻译";
    case SUITE_TRANSLATION_UNKNOWN: return "服务结果未知，请稍后";
    case SUITE_TRANSLATION_BUSY: return "本机记录已满（8/8）";
    case SUITE_TRANSLATION_UNAVAILABLE: return "翻译任务不可用";
    default: return "请选择翻译";
    }
}

static const char *hint_text(suite_translation_state_t state, bool busy,
                             bool reset_confirm)
{
    if (s_config_open) return "USB配置中  长按返回取消";
    if (state == SUITE_TRANSLATION_RECORDING)
        return "正在录音  OK结束  UP取消  长按返回";
    if (busy || state == SUITE_TRANSLATION_WORKING)
        return "处理中  UP/OK取消  长按返回";
    if (state == SUITE_TRANSLATION_IDLE)
        return "上下选择  OK确认  长按返回";
    if (state == SUITE_TRANSLATION_BUSY)
        return reset_confirm ? "再按OK确认清除  长按返回"
                             : "OK清除记录  长按返回";
    return "上下滚动  OK返回  长按返回";
}

static const char *mode_text(suite_translation_mode_t mode)
{
    return mode == SUITE_TRANSLATION_MODE_REAL ? "MiMo 联网"
                                               : "MOCK 本机合成";
}

static const char *row_mode_text(suite_translation_mode_t mode)
{
    return mode == SUITE_TRANSLATION_MODE_REAL ? "当前 MiMo 联网"
                                               : "当前 MOCK 本机";
}

static void update_rows(const suite_translation_view_t *view)
{
    lv_label_set_text(s_row_labels[3], row_mode_text(view->mode));
    lv_label_set_text(s_mode_label, mode_text(view->mode));
    lv_obj_set_style_text_color(s_mode_label,
                                lv_color_hex(view->mode ==
                                                     SUITE_TRANSLATION_MODE_MOCK
                                                 ? C_WARN
                                                 : C_INK),
                                0);
    for (unsigned i = 0; i < ROW_COUNT; ++i) {
        bool selected = i == s_choice;
        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex(selected ? C_SEL : C_CARD), 0);
        lv_obj_set_style_text_color(s_row_labels[i],
                                    lv_color_hex(selected ? C_INK : C_DIM), 0);
    }
}

/*
 * Runs in the LVGL timer context or while the caller already holds the LVGL
 * lock. It takes only the short translation snapshot lock.
 */
static void translation_tick(lv_timer_t *timer)
{
    (void)timer;
    suite_translation_poll();
    const suite_translation_view_t *view = suite_translation_view_acquire();
    if (!view) return;
    if (view->state != SUITE_TRANSLATION_BUSY) s_reset_confirm = false;
    if (s_config_open && !suite_translation_usb_window_open()) {
        s_config_open = false;
        s_serial = 0;
    }
    update_rows(view);
    int64_t now = esp_timer_get_time();
    if (s_config_open) {
        int64_t elapsed = (now - s_config_started_us) / 1000000;
        int remaining = USB_WINDOW_SECONDS - (int)elapsed;
        if (remaining < 0) remaining = 0;
        char text[48];
        snprintf(text, sizeof(text), "USB配置中 %d秒", remaining);
        lv_label_set_text(s_status, text);
        lv_label_set_text(s_hint, hint_text(view->state, view->worker_busy,
                                            s_reset_confirm));
        s_loaded = false;
    } else if (s_config_note[0] && now < s_config_note_until_us) {
        lv_label_set_text(s_status, s_config_note);
        lv_label_set_text(s_hint, "长按返回");
        s_loaded = false;
    } else {
        s_config_note[0] = 0;
        if (!s_loaded || s_serial != view->serial) {
            lv_label_set_text(s_status, view->status[0] ? view->status
                                                        : fallback_status(view->state));
            if (view->tls_diag.valid)
                show_tls_diagnostic(&view->tls_diag);
            else
                show_translation_text(view);
            s_serial = view->serial;
            s_loaded = true;
        }
        lv_label_set_text(s_hint, hint_text(view->state, view->worker_busy,
                                            s_reset_confirm));
    }
    suite_ui_battery_update(s_battery);
    suite_translation_view_release();
}

static void translation_enter(void)
{
    (void)suite_translation_start();
    (void)suite_translation_select_mode(SUITE_TRANSLATION_MODE_REAL);
    suite_translation_usb_set_busy_check(suite_translation_busy);
    suite_translation_session_enter();
    s_loaded = false;
    s_serial = 0;
    s_choice = 0;
    s_reset_confirm = false;
    s_config_open = false;
    s_config_note[0] = 0;
    s_config_note_until_us = 0;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = static_label(s_screen, "翻译", C_INK);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    s_battery = suite_ui_battery_create(s_screen);
    s_mode_label = static_label(s_screen, "MiMo 联网", C_INK);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_LEFT, 12, 28);

    static const char *labels[ROW_COUNT] = {
        "EN 译 中文", "中文 译 EN", "USB 配置", "当前 MiMo 联网",
    };
    for (unsigned i = 0; i < ROW_COUNT; ++i) {
        s_rows[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_rows[i], 216, 26);
        lv_obj_align(s_rows[i], LV_ALIGN_TOP_MID, 0, 48 + (int)i * 28);
        lv_obj_set_style_border_width(s_rows[i], 0, 0);
        lv_obj_set_style_pad_all(s_rows[i], 3, 0);
        lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        s_row_labels[i] = static_label(s_rows[i], labels[i], C_INK);
        lv_obj_align(s_row_labels[i], LV_ALIGN_LEFT_MID, 2, 0);
    }

    s_status = static_label(s_screen, "请选择翻译", C_DIM);
    lv_obj_set_width(s_status, 216);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 12, 162);

    s_body = lv_obj_create(s_screen);
    lv_obj_set_size(s_body, 216, 58);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 206);
    lv_obj_set_style_bg_color(s_body, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 6, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    s_source_caption = static_label(s_body, "源文", C_DIM);
    s_source = body_label(s_body, "无", C_INK);
    s_target_caption = static_label(s_body, "译文", C_DIM);
    s_target = body_label(s_body, "无", C_INK);

    s_hint = static_label(s_screen, "上下选择  OK确认  长按返回", C_DIM);
    lv_obj_set_width(s_hint, 216);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    s_timer = lv_timer_create(translation_tick, 250, NULL);
    lv_screen_load(s_screen);
    translation_tick(NULL);
}

static void translation_exit(void)
{
    if (s_config_open) {
        suite_translation_usb_cancel();
        s_config_open = false;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    /* Invalidate late worker results before destroying any UI object. */
    suite_translation_session_exit();
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_status = NULL;
    s_body = NULL;
    s_source = NULL;
    s_target = NULL;
    s_source_caption = NULL;
    s_target_caption = NULL;
    s_hint = NULL;
    s_battery = NULL;
    s_mode_label = NULL;
    for (unsigned i = 0; i < ROW_COUNT; ++i) {
        s_rows[i] = NULL;
        s_row_labels[i] = NULL;
    }
    s_loaded = false;
    s_serial = 0;
    s_reset_confirm = false;
}

static void open_config_window(void)
{
    int64_t now = esp_timer_get_time();
    if (suite_translation_usb_open_window()) {
        s_config_open = true;
        s_config_started_us = now;
        s_config_note[0] = 0;
    } else {
        snprintf(s_config_note, sizeof(s_config_note), "USB配置不可用");
        s_config_note_until_us = now + 2000000;
    }
}

static void translation_action(suite_action_t action)
{
    if (!bsp_lvgl_lock(100)) return;
    const suite_translation_view_t *view = suite_translation_view_acquire();
    if (!view) {
        bsp_lvgl_unlock();
        return;
    }
    suite_translation_state_t state = view->state;
    bool busy = view->worker_busy;
    tr_direction_t direction = view->direction;
    suite_translation_mode_t mode = view->mode;
    suite_translation_view_release();

    if (s_config_open) {
        bsp_lvgl_unlock();
        return;
    }

    bool select_direction = false;
    tr_direction_t next = direction;
    bool move = false;
    int delta = 0;
    bool scroll = false;
    bool run = false;
    bool cancel = false;
    bool finish = false;
    bool ready = false;
    bool reset = false;
    bool toggle_mode = false;
    bool config = false;

    if (state == SUITE_TRANSLATION_RECORDING) {
        if (action == SUITE_ACTION_OK) finish = true;
        else if (action == SUITE_ACTION_UP) cancel = true;
    } else if (busy || state == SUITE_TRANSLATION_WORKING) {
        if (action == SUITE_ACTION_UP || action == SUITE_ACTION_OK)
            cancel = true;
    } else if (state == SUITE_TRANSLATION_IDLE) {
        if (action == SUITE_ACTION_UP || action == SUITE_ACTION_DOWN) {
            if (action == SUITE_ACTION_UP)
                s_choice = s_choice ? s_choice - 1 : ROW_COUNT - 1;
            else
                s_choice = (s_choice + 1) % ROW_COUNT;
            move = true;
        } else if (action == SUITE_ACTION_OK) {
            if (s_choice == 0 || s_choice == 1) {
                next = s_choice == 0 ? TR_DIRECTION_EN_ZH
                                     : TR_DIRECTION_ZH_EN;
                select_direction = true;
                run = true;
            } else if (s_choice == 2) {
                config = true;
            } else {
                toggle_mode = true;
            }
        }
    } else if (state == SUITE_TRANSLATION_BUSY) {
        if (action == SUITE_ACTION_OK) {
            if (s_reset_confirm) {
                s_reset_confirm = false;
                reset = true;
            } else {
                s_reset_confirm = true;
            }
        } else if (action == SUITE_ACTION_UP || action == SUITE_ACTION_DOWN) {
            s_reset_confirm = false;
        }
    } else if (action == SUITE_ACTION_UP || action == SUITE_ACTION_DOWN) {
        scroll = true;
        delta = action == SUITE_ACTION_UP ? 31 : -31;
    } else if (action == SUITE_ACTION_OK) {
        ready = true;
    }
    bsp_lvgl_unlock();

    if (select_direction) (void)suite_translation_select(next);
    if (run) (void)suite_translation_run();
    else if (finish) suite_translation_finish_recording();
    else if (cancel) suite_translation_cancel();
    else if (reset) (void)suite_translation_reset_mock_session();
    else if (toggle_mode) {
        (void)suite_translation_select_mode(
            mode == SUITE_TRANSLATION_MODE_REAL
                ? SUITE_TRANSLATION_MODE_MOCK
                : SUITE_TRANSLATION_MODE_REAL);
    } else if (config) {
        open_config_window();
    } else if (scroll) {
        if (bsp_lvgl_lock(100)) {
            lv_obj_scroll_by(s_body, 0, delta, LV_ANIM_OFF);
            bsp_lvgl_unlock();
        }
    } else if (ready) {
        suite_translation_ready();
    }
    (void)move;
    if (bsp_lvgl_lock(100)) {
        translation_tick(NULL);
        bsp_lvgl_unlock();
    }
}

const suite_page_t SUITE_PAGE_TRANSLATION = {
    .name = "translation",
    .enter = translation_enter,
    .exit = translation_exit,
    .action = translation_action,
};
