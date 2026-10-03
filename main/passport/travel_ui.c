/*
 * travel_ui.c — travel application screens (D1 subset).
 *
 * Pages implemented with LV_GL primitives only; the visual design is new:
 * 28 px status strip, action-hint strip, centered content region, 12 px
 * side margins, 20 px titles / 16 px body (PRD proposal), explicit empty
 * states and a clear TEST DATA marker on the fixture detail page.
 */
#include "travel_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

#include "passport_text.h"
#include "travel_state.h"

LV_FONT_DECLARE(travel_font_16);
LV_FONT_DECLARE(travel_font_20);

enum {
    STRIP_H = 28,
    MARGIN = 12,
};

struct page_ui {
    lv_obj_t *screen;
    lv_obj_t *status;
    lv_obj_t *content;
    lv_obj_t *hint;
    lv_obj_t *list;         /* P01..P04 / P00 rows */
    lv_obj_t *detail_body;  /* P05 */
};

struct travel_ui_ctx {
    struct travel_state *state;
    struct page_ui pages[TRAVEL_PAGE_COUNT];
    lv_obj_t *battery_label;
    char battery_text[16];
    char row_v_text[32];
};

static void page_build(struct page_ui *page)
{
    lv_obj_t *parent = NULL;
    page->screen = lv_obj_create(NULL);
    lv_obj_set_size(page->screen, 240, 320);
    lv_obj_set_style_bg_color(page->screen, lv_color_hex(0x101418),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(page->screen,
                                lv_color_hex(0xF2F2F2), 0);
    lv_obj_set_style_pad_all(page->screen, 0, 0);
    lv_obj_set_style_border_width(page->screen, 0, 0);

    page->status = lv_label_create(page->screen);
    lv_obj_set_pos(page->status, MARGIN, 2);
    lv_obj_set_style_text_font(page->status, &travel_font_20, 0);

    page->content = lv_obj_create(page->screen);
    lv_obj_set_pos(page->content, MARGIN, STRIP_H);
    lv_obj_set_size(page->content, 240 - 2 * MARGIN, 320 - STRIP_H * 2);
    lv_obj_set_style_bg_color(page->content, lv_color_hex(0x1B2129), 0);
    lv_obj_set_style_text_color(page->content, lv_color_hex(0xF2F2F2), 0);
    lv_obj_set_style_pad_all(page->content, 4, 0);
    lv_obj_set_style_border_width(page->content, 0, 0);
    lv_obj_set_style_radius(page->content, 6, 0);

    page->hint = lv_label_create(page->screen);
    lv_obj_set_pos(page->hint, MARGIN, 320 - STRIP_H);
    lv_obj_set_style_text_font(page->hint, &travel_font_16, 0);
    lv_obj_set_style_text_color(page->hint, lv_color_hex(0x9AA3AD), 0);

    page->list = lv_label_create(page->content);
    lv_obj_set_style_text_font(page->list, &travel_font_16, 0);
    lv_label_set_long_mode(page->list, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(page->list, 240 - 2 * MARGIN - 8);

    page->detail_body = lv_label_create(page->content);
    lv_obj_set_style_text_font(page->detail_body, &travel_font_16, 0);
    lv_label_set_long_mode(page->detail_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(page->detail_body, 240 - 2 * MARGIN - 8);
}

static void hide_all(struct travel_ui_ctx *ctx)
{
    for (int i = 0; i < TRAVEL_PAGE_COUNT; ++i) {
        if (ctx->pages[i].screen)
            lv_obj_add_flag(ctx->pages[i].screen, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_status_hints(struct page_ui *page, const char *title,
                             const char *hint)
{
    lv_label_set_text(page->status, title);
    lv_label_set_text(page->hint, hint);
}

static void battery_refresh(struct travel_ui_ctx *ctx)
{
    struct travel_state *state = ctx->state;
    int soc = ctx->battery_label ? bsp_battery_soc() : -1;
    if (soc < 0) {
        lv_obj_add_flag(ctx->battery_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(ctx->battery_label, LV_OBJ_FLAG_HIDDEN);
    snprintf(ctx->battery_text, sizeof(ctx->battery_text), "%d%%", soc);
    lv_obj_set_pos(ctx->battery_label, 240 - MARGIN - 40, 2 + 4);
    lv_label_set_text(ctx->battery_label, ctx->battery_text);
}

static void render_home(struct travel_ui_ctx *ctx)
{
    struct page_ui *page = &ctx->pages[TRAVEL_P00_HOME];
    struct travel_state *state = ctx->state;
    set_status_hints(page, PASSPORT_STR_APP_TITLE, PASSPORT_STR_BACK);
    static char body[192];
    snprintf(body, sizeof(body),
             "%s\n\n%s\n%s: %s\n%s: %lu",
             PASSPORT_STR_JOURNEY "\xef\xbc\x9a" PASSPORT_FIXTURE_ID,
             state->pack_ready ? PASSPORT_STR_READY : PASSPORT_STR_NO_PACK,
             PASSPORT_STR_NEXT_STOP,
             state->stop_label[0] ? state->stop_label
                                  : PASSPORT_STR_NO_PACK,
             PASSPORT_STR_PENDING,
             (unsigned long)state->pending_events);
    lv_label_set_text(page->detail_body, body);
}

static void render_list_page(struct travel_ui_ctx *ctx, int id)
{
    struct page_ui *page = &ctx->pages[id];
    struct travel_state *state = ctx->state;
    /* four rows; selected row marked with '>' */
    static char rows[384];
    rows[0] = '\0';
    const char *titles[4] = {"", "", "", ""};
    uint8_t count = travel_state_menu_size(state);
    for (uint8_t i = 0; i < count; ++i) {
        int row_text = 0;
        switch (id) {
        case TRAVEL_P01_LAUNCHER:
            titles[i] = i == 0 ? PASSPORT_STR_GUIDE :
                i == 1 ? PASSPORT_STR_TICKETS :
                i == 2 ? PASSPORT_STR_SYNC : PASSPORT_STR_SETTINGS;
            break;
        case TRAVEL_P02_SYNC:
            titles[i] = i == 0 ? PASSPORT_STR_RESYNC :
                i == 1 ? PASSPORT_STR_BACK : PASSPORT_STR_CLOSE;
            break;
        case TRAVEL_P03_SETTINGS:
            titles[i] = i == 0 ? PASSPORT_STR_PROVISION :
                i == 1 ? PASSPORT_STR_CLEAR_NET : PASSPORT_STR_BACK;
            break;
        case TRAVEL_P04_MENU:
            titles[i] = i == 0 ? PASSPORT_STR_BACK :
                i == 1 ? PASSPORT_STR_RESYNC : PASSPORT_STR_CLOSE;
            break;
        default:
            break;
        }
        (void)row_text;
        snprintf(rows + strlen(rows), sizeof(rows) - strlen(rows),
                 "%s %s\n", state->menu_index == i ? ">" : "  ", titles[i]);
    }
    lv_label_set_text(page->list, rows);
}

static void render_detail(struct travel_ui_ctx *ctx)
{
    struct page_ui *page = &ctx->pages[TRAVEL_P05_DETAIL];
    struct travel_state *state = ctx->state;
    set_status_hints(page, PASSPORT_STR_DETAIL, PASSPORT_STR_TEST_DATA);
    static char body[512];
    snprintf(body, sizeof(body),
             "%s\n%s: %s\n\n%s\n\n%s: %s",
             state->stop_label[0] ? state->stop_label
                                  : PASSPORT_STR_FIXTURE_STOP_001_NAME,
             PASSPORT_STR_TEST_DATA, PASSPORT_FIXTURE_ID,
             PASSPORT_STR_FIXTURE_STOP_001_SUMMARY,
             PASSPORT_STR_STOP_COMPLETED,
             state->stop_completed ? PASSPORT_STR_ACK : PASSPORT_STR_QUEUED);
    lv_label_set_text(page->detail_body, body);
}

void travel_ui_render(struct travel_ui *ui)
{
    struct travel_ui_ctx *ctx = ui->ctx;
    if (!ctx)
        return;
    hide_all(ctx);
    battery_refresh(ctx);
    struct page_ui *page = &ctx->pages[ctx->state->page];
    lv_obj_clear_flag(page->screen, LV_OBJ_FLAG_HIDDEN);
    switch (ctx->state->page) {
    case TRAVEL_P00_HOME:
        render_home(ctx);
        break;
    case TRAVEL_P05_DETAIL:
        render_detail(ctx);
        break;
    default:
        render_list_page(ctx, ctx->state->page);
        break;
    }
}

int travel_ui_init(struct travel_ui *ui, struct travel_state *state)
{
    struct travel_ui_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->state = state;
    for (int i = 0; i < TRAVEL_PAGE_COUNT; ++i)
        page_build(&ctx->pages[i]);
    ctx->battery_label = lv_label_create(ctx->pages[0].screen);
    lv_obj_set_style_text_font(ctx->battery_label, &travel_font_16, 0);
    ui->ctx = ctx;
    travel_ui_render(ui);
    return 0;
}

void travel_ui_deinit(struct travel_ui *ui)
{
    struct travel_ui_ctx *ctx = ui->ctx;
    if (!ctx)
        return;
    for (int i = 0; i < TRAVEL_PAGE_COUNT; ++i) {
        if (ctx->pages[i].screen)
            lv_obj_delete(ctx->pages[i].screen);
    }
    free(ctx);
    ui->ctx = NULL;
}
