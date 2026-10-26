#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { int line_height; } lv_font_t;
typedef uint32_t lv_color_t;
typedef struct test_obj {
    struct test_obj *parent;
    struct test_obj *children[16];
    unsigned count;
    char text[1024];
    int width,height,x,y,pad,scroll,bottom;
    uint32_t bg,flags;
    const lv_font_t *font;
} lv_obj_t;
typedef struct test_timer { void (*callback)(struct test_timer *); } lv_timer_t;
enum { LV_ALIGN_TOP_LEFT,LV_ALIGN_TOP_MID,LV_ALIGN_BOTTOM_LEFT,
       LV_OBJ_FLAG_SCROLLABLE,LV_LABEL_LONG_WRAP,LV_LABEL_LONG_DOT,
       LV_FLEX_FLOW_COLUMN,LV_DIR_VER,LV_ANIM_OFF };
#define LV_OBJ_FLAG_HIDDEN (1U << 8)
#define LV_ALIGN_RIGHT_MID 100
#define LV_ALIGN_LEFT_MID 101
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_set_style_text_font(lv_obj_t *,const lv_font_t *,int);
void lv_obj_set_style_text_color(lv_obj_t *,lv_color_t,int);
void lv_label_set_text(lv_obj_t *,const char *);
void lv_obj_set_style_bg_color(lv_obj_t *,lv_color_t,int);
void lv_obj_clean(lv_obj_t *);
void lv_obj_delete(lv_obj_t *);
void lv_obj_set_width(lv_obj_t *,int);
void lv_obj_set_size(lv_obj_t *,int,int);
void lv_label_set_long_mode(lv_obj_t *,int);
void lv_obj_align(lv_obj_t *,int,int,int);
void lv_obj_remove_flag(lv_obj_t *,int);
void lv_obj_add_flag(lv_obj_t *,int);
void lv_obj_set_style_border_width(lv_obj_t *,int,int);
void lv_obj_set_style_pad_all(lv_obj_t *,int,int);
void lv_obj_set_flex_flow(lv_obj_t *,int);
void lv_obj_set_scroll_dir(lv_obj_t *,int);
void lv_obj_scroll_to_y(lv_obj_t *,int,int);
void lv_obj_scroll_by(lv_obj_t *,int,int,int);
int lv_obj_get_scroll_y(lv_obj_t *);
int lv_obj_get_scroll_bottom(lv_obj_t *);
lv_timer_t *lv_timer_create(void (*)(lv_timer_t *),unsigned,void *);
void lv_timer_delete(lv_timer_t *);
void lv_screen_load(lv_obj_t *);
static inline lv_color_t lv_color_hex(uint32_t color) { return color; }
