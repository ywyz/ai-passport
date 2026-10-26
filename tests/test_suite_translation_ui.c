/* Exercise the real translation page, not a copy of its input dispatch. */
#include "../main/suite_translation_ui.c"

#include <assert.h>
#include <stdlib.h>

const lv_font_t suite_font_16 = { .line_height = 20 };
const lv_font_t suite_tasks_font_16 = { .line_height = 31 };

static suite_translation_view_t view;
static const suite_page_t *page;
static unsigned objects, timers, starts, enters, exits, selects, runs, cancels,
    readys, polls, resets, modes, finishes, usb_opens, usb_cancels;
static bool locked, borrowed, usb_open_ok, usb_window_open;
static int64_t s_time_us = 1000000;

bool bsp_lvgl_lock(unsigned timeout)
{
    (void)timeout; assert(!locked); locked = true; return true;
}
void bsp_lvgl_unlock(void) { assert(locked); locked = false; }
esp_err_t suite_translation_start(void) { starts++; return ESP_OK; }
void suite_translation_session_enter(void) { enters++; view.session++; }
void suite_translation_session_exit(void) { exits++; view.session++; }
bool suite_translation_select(tr_direction_t direction)
{
    if (view.worker_busy) return false;
    selects++; view.direction = direction; view.serial++; return true;
}
bool suite_translation_select_mode(suite_translation_mode_t mode)
{
    if (view.worker_busy) return false;
    modes++;
    view.mode = mode;
    view.mock = mode == SUITE_TRANSLATION_MODE_MOCK;
    view.serial++;
    return true;
}
bool suite_translation_run(void)
{
    if (view.worker_busy) return false;
    runs++;
    view.state = SUITE_TRANSLATION_WORKING;
    view.worker_busy = true;
    view.source[0] = 0; view.target[0] = 0;
    strcpy(view.status, view.mode == SUITE_TRANSLATION_MODE_REAL
                            ? "正在准备联网翻译" : "正在准备本机输入");
    view.serial++;
    return true;
}
void suite_translation_finish_recording(void) { finishes++; }
bool suite_translation_busy(void) { return view.worker_busy; }
void suite_translation_cancel(void)
{
    cancels++;
    view.state = SUITE_TRANSLATION_CANCELLED;
    view.source[0] = 0; view.target[0] = 0;
    strcpy(view.status, "正在取消任务");
    view.serial++;
}
void suite_translation_ready(void)
{
    readys++;
    if (view.worker_busy) return;
    view.state = SUITE_TRANSLATION_IDLE;
    view.source[0] = 0; view.target[0] = 0;
    strcpy(view.status, "请选择翻译");
    view.serial++;
}
bool suite_translation_reset_mock_session(void)
{
    resets++;
    view.state = SUITE_TRANSLATION_IDLE;
    view.source[0] = 0; view.target[0] = 0;
    view.identity_epoch++;
    strcpy(view.status, "本机记录已清除，可继续");
    view.serial++;
    return true;
}
void suite_translation_poll(void) { polls++; }
bool suite_translation_set_provider(const suite_translation_provider_t *provider)
{
    (void)provider; return true;
}
const suite_translation_view_t *suite_translation_view_acquire(void)
{
    assert(!borrowed); borrowed = true; return &view;
}
void suite_translation_view_release(void) { assert(borrowed); borrowed = false; }

bool suite_translation_usb_open_window(void)
{
    usb_opens++;
    if (!usb_open_ok) return false;
    usb_window_open = true;
    return true;
}
void suite_translation_usb_cancel(void) { usb_cancels++; usb_window_open = false; }
bool suite_translation_usb_window_open(void) { return usb_window_open; }
void suite_translation_usb_set_busy_check(bool (*busy)(void)) { (void)busy; }
int64_t esp_timer_get_time(void) { return s_time_us; }

lv_obj_t *suite_ui_battery_create(lv_obj_t *parent) { return lv_label_create(parent); }
void suite_ui_battery_update(lv_obj_t *object) { (void)object; }

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    lv_obj_t *object = calloc(1, sizeof(*object));
    assert(object); objects++;
    object->parent = parent; object->width = 240; object->height = 320;
    if (parent) {
        assert(parent->count < 16);
        parent->children[parent->count++] = object;
    }
    return object;
}
lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s)
{ (void)s; o->font = f; o->height = f->line_height; }
void lv_label_set_text(lv_obj_t *o, const char *text)
{ assert(strlen(text) < sizeof(o->text)); strcpy(o->text, text); }
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int s)
{ (void)o; (void)c; (void)s; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s)
{ (void)s; o->bg = c; }
void lv_obj_clean(lv_obj_t *o) { while (o->count) lv_obj_delete(o->children[--o->count]); }
void lv_obj_delete(lv_obj_t *o)
{
    lv_obj_clean(o);
    if (o->parent) for (unsigned i = 0; i < o->parent->count; ++i)
        if (o->parent->children[i] == o) {
            memmove(o->parent->children + i, o->parent->children + i + 1,
                    (o->parent->count - i - 1) * sizeof(o));
            o->parent->count--; break;
        }
    free(o); objects--;
}
void lv_obj_set_width(lv_obj_t *o, int w) { o->width = w; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->width = w; o->height = h; }
void lv_label_set_long_mode(lv_obj_t *o, int mode) { (void)o; (void)mode; }
void lv_obj_align(lv_obj_t *o, int align, int x, int y)
{
    o->x = x;
    o->y = align == LV_ALIGN_BOTTOM_LEFT ? 320 - o->height + y : y;
}
void lv_obj_remove_flag(lv_obj_t *o, int flag) { o->flags &= ~(unsigned)flag; }
void lv_obj_add_flag(lv_obj_t *o, int flag) { o->flags |= (unsigned)flag; }
void lv_obj_set_style_border_width(lv_obj_t *o, int w, int s)
{ (void)o; (void)w; (void)s; }
void lv_obj_set_style_pad_all(lv_obj_t *o, int pad, int s) { (void)s; o->pad = pad; }
void lv_obj_set_flex_flow(lv_obj_t *o, int flow) { (void)o; (void)flow; }
void lv_obj_set_scroll_dir(lv_obj_t *o, int dir) { (void)o; (void)dir; }
void lv_obj_scroll_to_y(lv_obj_t *o, int y, int anim) { (void)anim; o->scroll = y; }
void lv_obj_scroll_by(lv_obj_t *o, int x, int y, int anim)
{ (void)x; (void)anim; o->scroll -= y; o->bottom += y; }
int lv_obj_get_scroll_y(lv_obj_t *o) { return o->scroll; }
int lv_obj_get_scroll_bottom(lv_obj_t *o) { return o->bottom; }
lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), unsigned ms, void *data)
{ (void)ms; (void)data; lv_timer_t *t = malloc(sizeof(*t)); assert(t); timers++; t->callback = callback; return t; }
void lv_timer_delete(lv_timer_t *t) { free(t); timers--; }
void lv_screen_load(lv_obj_t *o) { (void)o; }

static void view_reset(void)
{
    memset(&view, 0, sizeof(view));
    view.state = SUITE_TRANSLATION_IDLE;
    view.direction = TR_DIRECTION_EN_ZH;
    view.mode = SUITE_TRANSLATION_MODE_REAL;
    view.mock = false;
    view.serial = 1;
    strcpy(view.status, "请选择翻译");
    starts = enters = exits = selects = runs = cancels = readys = polls = 0;
    resets = modes = finishes = usb_opens = usb_cancels = 0;
    locked = borrowed = false; usb_open_ok = true; usb_window_open = false;
    s_time_us = 1000000;
}
static void draw(void) { assert(s_timer); s_timer->callback(s_timer); assert(!locked && !borrowed); }
static void act(suite_action_t action) { page->action(action); assert(!locked && !borrowed); draw(); }
static void enter(void)
{
    assert(!objects && !timers);
    view_reset();
    page = &SUITE_PAGE_TRANSLATION;
    page->enter();
    assert(objects > 0 && timers == 1);
    draw();
}
static void leave(void)
{
    page->exit();
    assert(!objects && !timers && !locked && !borrowed);
    assert(s_screen == NULL && s_timer == NULL && s_source == NULL && s_target == NULL);
}

int main(void)
{
    enter();
    assert(starts == 1 && enters == 1);
    assert(modes == 1 && view.mode == SUITE_TRANSLATION_MODE_REAL);
    assert(!strcmp(s_mode_label->text, "MiMo 联网"));
    assert(!strcmp(s_row_labels[3]->text, "当前 MiMo 联网"));
    assert(s_row_labels[0]->font == &suite_font_16);
    assert(s_source->font == &suite_tasks_font_16);
    assert(s_target->font == &suite_tasks_font_16);
    assert(s_rows[3]->y + s_rows[3]->height <= s_status->y);
    assert(s_status->y + 2 * s_status->font->line_height <= s_body->y);
    int hint_top = 320 - 2 * s_hint->font->line_height - 8;
    assert(s_body->y + s_body->height <= hint_top);
    assert(s_source->width <= s_body->width - 2 * s_body->pad);

    /* Mode row is explicit. */
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_DOWN);
    assert(s_choice == 3);
    act(SUITE_ACTION_OK);
    assert(modes >= 2 && view.mode == SUITE_TRANSLATION_MODE_MOCK);
    assert(!strcmp(s_mode_label->text, "MOCK 本机合成"));
    act(SUITE_ACTION_OK);
    assert(view.mode == SUITE_TRANSLATION_MODE_REAL);

    /* Direction row starts the selected real mode. */
    act(SUITE_ACTION_UP); act(SUITE_ACTION_UP); act(SUITE_ACTION_UP);
    assert(s_choice == 0);
    act(SUITE_ACTION_OK);
    assert(runs == 1 && selects >= 1 && view.state == SUITE_TRANSLATION_WORKING);
    assert(strstr(s_hint->text, "UP/OK取消"));
    act(SUITE_ACTION_UP);
    assert(cancels == 1);

    /* Recording: OK finishes, UP cancels. */
    view.state = SUITE_TRANSLATION_RECORDING;
    view.worker_busy = true;
    view.serial++;
    draw();
    assert(strstr(s_hint->text, "OK结束"));
    act(SUITE_ACTION_OK);
    assert(finishes == 1);
    act(SUITE_ACTION_UP);
    assert(cancels >= 2);
    view.worker_busy = false;
    view.state = SUITE_TRANSLATION_CANCELLED;
    view.serial++;
    draw();

    /* Result: separate panes and return to ready. */
    memset(view.source, 'A', 300); view.source[300] = 0;
    memset(view.target, 'B', 300); view.target[300] = 0;
    strcpy(view.status, "翻译完成");
    view.state = SUITE_TRANSLATION_DONE;
    view.serial++;
    draw();
    assert(!strcmp(s_source->text, view.source));
    assert(!strcmp(s_target->text, view.target));
    assert(strstr(s_hint->text, "上下滚动"));
    int before_scroll = s_body->scroll;
    act(SUITE_ACTION_DOWN);
    assert(s_body->scroll - before_scroll == 31);
    int stable_scroll = s_body->scroll;
    draw();
    assert(s_body->scroll == stable_scroll);
    act(SUITE_ACTION_OK);
    assert(readys >= 1 && view.state == SUITE_TRANSLATION_IDLE);

    /* Config row opens the USB window; exiting cancels it. */
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_DOWN);
    assert(s_choice == 2);
    act(SUITE_ACTION_OK);
    assert(usb_opens == 1 && s_config_open && usb_window_open);
    assert(strstr(s_status->text, "USB配置中"));
    leave();
    assert(usb_cancels == 1);

    /* Failure, unknown, expired, mock 8/8, unavailable. */
    enter();
    strcpy(view.source, "source"); view.target[0] = 0;
    strcpy(view.status, "翻译失败，源文已保留");
    view.state = SUITE_TRANSLATION_FAILED; view.serial++; draw();
    assert(!strcmp(s_source->text, "source") && !strcmp(s_target->text, "无"));
    act(SUITE_ACTION_OK);
    strcpy(view.status, "服务结果未知，请稍后");
    view.state = SUITE_TRANSLATION_UNKNOWN; view.serial++; draw();
    assert(strstr(s_status->text, "未知"));
    act(SUITE_ACTION_OK);
    strcpy(view.status, "结果已过期，请重新翻译");
    view.state = SUITE_TRANSLATION_EXPIRED; view.serial++; draw();
    assert(strstr(s_status->text, "过期"));
    act(SUITE_ACTION_OK);
    strcpy(view.status, "本机记录已满（8/8），可清除本机记录");
    view.state = SUITE_TRANSLATION_BUSY; view.serial++; draw();
    assert(strstr(s_hint->text, "OK清除记录"));
    act(SUITE_ACTION_OK);
    assert(s_reset_confirm && resets == 0);
    act(SUITE_ACTION_OK);
    assert(resets == 1 && view.state == SUITE_TRANSLATION_IDLE);
    strcpy(view.status, "翻译未配置，请用USB配置");
    view.state = SUITE_TRANSLATION_UNAVAILABLE; view.serial++; draw();
    assert(strstr(s_status->text, "USB配置"));

    /* TLS preflight diagnostic replaces the body values and hides captions. */
    memset(&view.tls_diag, 0, sizeof(view.tls_diag));
    view.tls_diag.valid = true;
    view.tls_diag.stage = SUITE_TR_TLS_STAGE_TCP_SO_ERROR;
    view.tls_diag.sys_source = SUITE_TR_TLS_SYS_SO_ERROR;
    view.tls_diag.so_error = 111;
    view.tls_diag.sys_error = 111;
    view.tls_diag.esp_err_code = 0x3000;
    view.tls_diag.tls_code = -0x7F00;
    view.tls_diag.verify_flags = 0x8000;
    view.tls_diag.free_bytes = 50000;
    view.tls_diag.largest_bytes = 4000;
    strcpy(view.status, "翻译服务不可用\nR7 S8 X0");
    view.state = SUITE_TRANSLATION_FAILED; view.serial++; draw();
    assert(!strcmp(s_source->text,
                   "E3000 T-7F00 V8000\nN111 F50000 L4000"));
    assert(s_source->font == &suite_font_16);
    assert(s_target->flags & LV_OBJ_FLAG_HIDDEN);
    assert(s_source_caption->flags & LV_OBJ_FLAG_HIDDEN);
    assert(s_target_caption->flags & LV_OBJ_FLAG_HIDDEN);

    /* Worst-case 32-bit values remain complete; wrapping is scroll-accessible. */
    view.tls_diag.esp_err_code = -1;
    view.tls_diag.tls_code = -2147483647 - 1;
    view.tls_diag.verify_flags = 0xFFFFFFFFU;
    view.tls_diag.stage = 12;
    view.tls_diag.local_errno = -2147483647 - 1;
    view.tls_diag.sys_error = -2147483647 - 1;
    view.tls_diag.free_bytes = 0xFFFFFFFFU;
    view.tls_diag.largest_bytes = 0xFFFFFFFFU;
    strcpy(view.status, "翻译服务不可用\nR7 S12 X-2147483648");
    view.serial++; draw();
    assert(!strcmp(s_status->text, "翻译服务不可用\nR7 S12 X-2147483648"));
    assert(!strcmp(s_source->text,
                   "EFFFFFFFF T-80000000 VFFFFFFFF\n"
                   "N-2147483648 F4294967295 L4294967295"));
    assert(s_body->scroll == 0);
    act(SUITE_ACTION_DOWN);
    int diag_scroll = s_body->scroll;
    assert(diag_scroll != 0);
    draw();
    assert(s_body->scroll == diag_scroll);

    /* Ordinary result restores captions, font and text. */
    memset(&view.tls_diag, 0, sizeof(view.tls_diag));
    strcpy(view.source, "source"); strcpy(view.target, "target");
    strcpy(view.status, "翻译完成");
    view.state = SUITE_TRANSLATION_DONE; view.serial++; draw();
    assert(!(s_source_caption->flags & LV_OBJ_FLAG_HIDDEN));
    assert(!(s_target_caption->flags & LV_OBJ_FLAG_HIDDEN));
    assert(!(s_target->flags & LV_OBJ_FLAG_HIDDEN));
    assert(s_source->font == &suite_tasks_font_16);
    assert(s_target->font == &suite_tasks_font_16);
    assert(!strcmp(s_source->text, "source"));
    assert(!strcmp(s_target->text, "target"));
    leave();
    puts("real translation page modes/config/recording/panes/lifecycle: PASS");
    return 0;
}
