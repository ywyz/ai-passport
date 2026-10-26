#include "suite_tasks.h"
#include "suite_pages.h"
#include "suite_connection.h"
#include "suite_clock.h"
#include "suite_tasks_nav.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "fonts/suite_fonts.h"
#include <stdio.h>
#include <string.h>

extern const lv_font_t suite_tasks_font_16;
#define BG 0x0e1420
#define CARD 0x1a2332
#define SELECT 0x29405b
#define INK 0xe8eef7
#define DIM 0x8ca0b8

static lv_obj_t *s_screen,*s_status,*s_receipt,*s_rows[3],*s_labels[3],*s_badges[3];
static lv_obj_t *s_refresh_row,*s_refresh_label,*s_battery,*s_body,*s_hint;
static lv_obj_t *s_voice_row,*s_voice_label;
static lv_timer_t *s_timer;
static unsigned s_selected,s_count,s_note,s_note_pages,s_serial;
static bool s_detail,s_busy,s_loaded;
static lv_obj_t *s_modal,*s_modal_hint,*s_choices[2],*s_choice_labels[2];
static unsigned s_dialog,s_choice;
static bool s_dialog_allowed,s_dialog_discard;
static char s_dialog_id[65],s_dialog_etag[257];

static lv_obj_t *content_label(lv_obj_t *p,const char *text,uint32_t color)
{
    lv_obj_t *o=lv_label_create(p);
    lv_obj_set_style_text_font(o,&suite_tasks_font_16,0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_label_set_text(o,text); return o;
}
static lv_obj_t *label(lv_obj_t *p,const char *text,uint32_t color)
{
    lv_obj_t *o=content_label(p,text,color);
    lv_obj_set_style_text_font(o,&suite_font_16,0);
    return o;
}
static const char *status_name(suite_tasks_state_t s)
{
    switch (s) {
    case TASKS_LOADING: return "正在加载缓存";
    case TASKS_UNCONFIGURED: return "请通过 USB 配置令牌";
    case TASKS_REFRESHING: return "正在下载，OK取消";
    case TASKS_OFFLINE: return "离线，使用缓存";
    case TASKS_TIME_REQUIRED: return "请先联网校时";
    case TASKS_AUTH_FAILED: return "令牌无效，请重新配置";
    case TASKS_SOURCE_AUTH_FAILED: return "服务端认证待修复";
    case TASKS_UNAVAILABLE: return "待办不可用，内存不足";
    case TASKS_FAILED: return "刷新失败，保留缓存";
    case TASKS_CANCELLED: return "已取消，保留缓存";
    case TASKS_STORAGE_FAILED: return "缓存异常，数据保留";
    default: return "只读";
    }
}
static const char *task_status(const char *status)
{
    if (!strcmp(status,"NEEDS-ACTION")) return "未完成";
    if (!strcmp(status,"IN-PROCESS")) return "进行中";
    if (!strcmp(status,"COMPLETED")) return "已完成";
    if (!strcmp(status,"CANCELLED")) return "已取消";
    return "状态未知";
}
static const char *operation_name(task_op_state_t state)
{
    switch (state) {
    case TASK_OP_LOCAL: return "完成意图已保存，待同步";
    case TASK_OP_SERVER: return "服务端已接收，正在同步";
    case TASK_OP_SUCCEEDED: return "已在 Nextcloud 完成";
    case TASK_OP_CONFLICT: return "远端已有修改，请刷新重确认";
    case TASK_OP_AUTH: return "完成认证待修复，再刷新";
    case TASK_OP_PAUSED: return "同步已暂停，手动刷新重试";
    case TASK_OP_READONLY: return "该待办为只读，无法完成";
    case TASK_OP_NOT_FOUND: return "远端待办已删除";
    case TASK_OP_FAILED: return "同步失败，手动刷新重试";
    default: return "OK打开操作，长按返回";
    }
}
static void receipt(char *out,size_t cap,const suite_tasks_view_t *v)
{
    if (!v->cached) { snprintf(out,cap,"尚无离线缓存"); return; }
    suite_civil_t server,device;
    suite_civil_from_epoch(v->server_at,8*3600,&server);
    suite_civil_from_epoch(v->device_at,8*3600,&device);
    snprintf(out,cap,"服务端 %02d/%02d %02d:%02d\n本机 %02d/%02d %02d:%02d UTC+8",
        server.month,server.day,server.hour,server.minute,device.month,device.day,device.hour,device.minute);
}
static void sync_selection(const suite_tasks_view_t *v)
{
    bool voice=s_selected==s_count+1;
    s_count=v->count; s_busy=v->state==TASKS_REFRESHING;
    if (voice) s_selected=s_count+1;
    else if (s_selected>s_count) s_selected=0;
}
static void tick(lv_timer_t *t)
{
    (void)t;
    /* Timer executes in LVGL context; input handlers explicitly lock below. */
    const suite_tasks_view_t *v=suite_tasks_view_acquire();
    if (!v) return;
    sync_selection(v);
    char text[128];
    const char *st=status_name(v->state);
    if (v->state==TASKS_READY) {
        if (suite_connection_state()!=SUITE_CONN_ONLINE) st=v->completion_configured ? "离线，可保存完成意图" : "离线，只读缓存";
        else if (v->stale) st="缓存过期，完成可能冲突";
        else st=v->count ? (v->completion_configured ? "已缓存，可确认完成" : "已缓存，只读") : "暂无未完成待办";
        if (v->pending_count) st="有完成请求待同步，请看详情";
    }
    if (v->notice[0] && v->state!=TASKS_REFRESHING) st=v->notice;
    if (!s_detail && v->state==TASKS_READY) {
        snprintf(text,sizeof(text),"%u条 %.96s",s_count,st); lv_label_set_text(s_status,text);
    } else lv_label_set_text(s_status,st);
    if (!s_detail) {
        receipt(text,sizeof(text),v); lv_label_set_text(s_receipt,text);
        unsigned index=s_selected && s_selected<=s_count ? s_selected-1 : 0;
        unsigned first=index/3*3;
        for (unsigned i=0;i<3;i++) {
            if (first+i>=s_count) {
                lv_obj_add_flag(s_rows[i],LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_labels[i],"");
                continue;
            }
            lv_obj_remove_flag(s_rows[i],LV_OBJ_FLAG_HIDDEN);
            task_op_state_t op=v->row_operations[i];
            const char *badge=op==TASK_OP_NONE ? "" : op==TASK_OP_SUCCEEDED ? "完成" : op==TASK_OP_CONFLICT ? "冲突" :
                (op==TASK_OP_LOCAL || op==TASK_OP_SERVER) ? "待传" : "暂停";
            lv_label_set_text(s_badges[i],badge);
            lv_obj_set_width(s_labels[i],op==TASK_OP_NONE ? 204 : 166);
            bool selected=s_selected && s_selected<=s_count && first+i==index && !s_busy;
            lv_obj_set_style_bg_color(s_rows[i],lv_color_hex(selected ? SELECT : CARD),0);
            if (v->selected/3==first/3)
                lv_label_set_text(s_labels[i],v->titles[i][strspn(v->titles[i]," \t\r\n")] ? v->titles[i] : "（无标题）");
            else lv_label_set_text(s_labels[i],"...");
        }
        if (s_busy) snprintf(text,sizeof(text),"OK 取消下载");
        else snprintf(text,sizeof(text),"刷新待办");
        lv_label_set_text(s_refresh_label,text);
        lv_obj_set_style_bg_color(s_refresh_row,lv_color_hex(!s_selected || s_busy ? SELECT : CARD),0);
        lv_obj_set_style_bg_color(s_voice_row,lv_color_hex(s_selected==s_count+1 && !s_busy ? SELECT : CARD),0);
        lv_label_set_text(s_hint,s_busy ? "OK取消下载  长按返回" : "上下选择 OK确认 长按返回");
    } else if (s_selected && v->detail && v->selected==s_selected-1 && (!s_loaded || s_serial!=v->serial)) {
        lv_obj_clean(s_body);
        char flags[100];
        snprintf(flags,sizeof(flags),"%s%s%s%s",v->request_only ? "保留请求" : task_status(v->status),v->recurring ? " / 重复任务" : "",
            v->parent ? " / 子任务" : "",v->truncated ? " / 备注已截断" : "");
        char collection[96]; snprintf(collection,sizeof(collection),"列表标识：%.64s",v->collection);
        const char *parts[]={v->titles[(s_selected-1)%3],flags,operation_name(v->operation),v->due,collection,
            v->note[0] ? v->note : "暂无备注"};
        for (unsigned i=0;i<6;i++) {
            lv_obj_t *l=(i==1 || i==2) ? label(s_body,parts[i],DIM) : content_label(s_body,parts[i],i==0 ? INK : DIM);
            lv_obj_set_width(l,196); lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
        }
        s_note=v->note_page; s_note_pages=v->note_pages;
        receipt(text,sizeof(text),v);
        lv_obj_t *r=label(s_body,text,DIM); lv_obj_set_width(r,196);
        snprintf(text,sizeof(text),"备注 %u/%u  OK操作 长按返回",s_note+1,s_note_pages);
        lv_label_set_text(s_receipt,text);
        lv_obj_scroll_to_y(s_body,0,LV_ANIM_OFF);
        s_loaded=true; s_serial=v->serial;
    }
    suite_ui_battery_update(s_battery);
    suite_tasks_view_release();
}

static void common_enter(bool detail)
{
    s_detail=detail; s_loaded=false; s_serial=0; s_dialog=0; s_modal=NULL;
    s_screen=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen,lv_color_hex(BG),0);
    lv_obj_remove_flag(s_screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title=label(s_screen,"待办",INK);
    lv_obj_align(title,LV_ALIGN_TOP_LEFT,12,8);
    s_battery=suite_ui_battery_create(s_screen);
    s_status=label(s_screen,"正在加载缓存",DIM); lv_obj_align(s_status,LV_ALIGN_TOP_LEFT,12,34);
    lv_obj_set_width(s_status,216); lv_label_set_long_mode(s_status,LV_LABEL_LONG_DOT);
    s_receipt=label(s_screen,"",DIM);
    if (detail) {
        lv_obj_align(s_receipt,LV_ALIGN_BOTTOM_LEFT,12,-8);
        s_body=lv_obj_create(s_screen); lv_obj_set_size(s_body,224,224);
        lv_obj_align(s_body,LV_ALIGN_TOP_MID,0,58);
        lv_obj_set_style_bg_color(s_body,lv_color_hex(CARD),0);
        lv_obj_set_style_border_width(s_body,0,0);
        lv_obj_set_style_pad_all(s_body,10,0);
        lv_obj_set_flex_flow(s_body,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_body,LV_DIR_VER);
    } else {
        lv_obj_align(s_receipt,LV_ALIGN_TOP_LEFT,12,246);
        s_refresh_row=lv_obj_create(s_screen);
        lv_obj_set_size(s_refresh_row,104,30); lv_obj_align(s_refresh_row,LV_ALIGN_TOP_LEFT,12,56);
        lv_obj_set_style_bg_color(s_refresh_row,lv_color_hex(SELECT),0);
        lv_obj_set_style_border_width(s_refresh_row,0,0);
        lv_obj_set_style_pad_all(s_refresh_row,4,0);
        lv_obj_remove_flag(s_refresh_row,LV_OBJ_FLAG_SCROLLABLE);
        s_refresh_label=label(s_refresh_row,"刷新待办",INK);
        s_voice_row=lv_obj_create(s_screen); lv_obj_set_size(s_voice_row,104,30);
        lv_obj_align(s_voice_row,LV_ALIGN_TOP_LEFT,124,56);
        lv_obj_set_style_bg_color(s_voice_row,lv_color_hex(CARD),0);
        lv_obj_set_style_border_width(s_voice_row,0,0); lv_obj_set_style_pad_all(s_voice_row,4,0);
        lv_obj_remove_flag(s_voice_row,LV_OBJ_FLAG_SCROLLABLE);
        s_voice_label=label(s_voice_row,"语音新建",INK);
        for (int i=0;i<3;i++) {
            s_rows[i]=lv_obj_create(s_screen);
            lv_obj_set_size(s_rows[i],216,49); lv_obj_align(s_rows[i],LV_ALIGN_TOP_MID,0,90+i*51);
            lv_obj_set_style_bg_color(s_rows[i],lv_color_hex(CARD),0);
            lv_obj_set_style_border_width(s_rows[i],0,0);
            lv_obj_set_style_pad_all(s_rows[i],6,0);
            lv_obj_remove_flag(s_rows[i],LV_OBJ_FLAG_SCROLLABLE);
            s_labels[i]=content_label(s_rows[i],"",INK);
            lv_obj_set_size(s_labels[i],204,33); lv_label_set_long_mode(s_labels[i],LV_LABEL_LONG_DOT);
            s_badges[i]=label(s_rows[i],"",DIM); lv_obj_set_width(s_badges[i],32);
            lv_obj_align(s_badges[i],LV_ALIGN_RIGHT_MID,0,0);
        }
        s_hint=label(s_screen,"上下选择 OK确认 长按返回",DIM);
        lv_obj_align(s_hint,LV_ALIGN_BOTTOM_LEFT,12,-10);
    }
    s_timer=lv_timer_create(tick,250,NULL);
    lv_screen_load(s_screen);
    suite_tasks_select(s_selected && s_selected<=s_count ? s_selected-1 : 0,detail,0);
}
static void list_enter(void)
{
    /* New list visits focus Refresh; returning from a detail keeps its item. */
    if (!s_detail) s_selected=0;
    common_enter(false);
}
static void detail_enter(void) { common_enter(true); }
static void exit_page(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer=NULL; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen=NULL; }
    s_modal=NULL; s_dialog=0;
    /* Worker owns no LVGL objects and can safely finish after page exit. */
}
static void list_action(suite_action_t a)
{
    if (!bsp_lvgl_lock(100)) return;
    const suite_tasks_view_t *v=suite_tasks_view_acquire();
    if (!v) { bsp_lvgl_unlock(); return; }
    sync_selection(v); suite_tasks_view_release();
    bool open=false,refresh=false,cancel=false,voice=false;
    if (a==SUITE_ACTION_UP) s_selected=task_list_move(s_selected,s_count+1,false);
    else if (a==SUITE_ACTION_DOWN) s_selected=task_list_move(s_selected,s_count+1,true);
    else if (a==SUITE_ACTION_OK) {
        if (s_busy) cancel=true;
        else if (!s_selected) refresh=true;
        else if (s_selected==s_count+1) voice=true;
        else open=true;
    }
    unsigned selected=s_selected && s_selected<=s_count ? s_selected-1 : 0;
    bsp_lvgl_unlock();
    if (cancel) suite_tasks_cancel();
    else if (refresh) suite_tasks_refresh();
    else if (voice) (void)suite_app_push(&SUITE_PAGE_TASK_CREATE);
    else if (open) (void)suite_app_push(&SUITE_PAGE_TASK_DETAIL);
    else suite_tasks_select(selected,false,0);
}
static void paint_dialog(void)
{
    lv_label_set_text(s_choice_labels[0],s_dialog==1 ? "返回列表" : "取消");
    lv_label_set_text(s_choice_labels[1],s_dialog==1 ? (s_dialog_discard ? "放弃旧完成请求" : "标记完成") :
                      s_dialog_discard ? "确认放弃" : "确认完成");
    for (unsigned i=0;i<2;i++) lv_obj_set_style_bg_color(s_choices[i],lv_color_hex(i==s_choice ? SELECT : BG),0);
    if (s_dialog==2) lv_label_set_text(s_modal_hint,s_dialog_discard ? "只放弃旧请求，不修改远端" : "确认后保存，再同步到远端");
}
static void open_dialog(const suite_tasks_view_t *v)
{
    strcpy(s_dialog_id,v->task_id); strcpy(s_dialog_etag,v->etag);
    s_dialog_discard=v->operation==TASK_OP_CONFLICT || v->operation==TASK_OP_READONLY || v->operation==TASK_OP_NOT_FOUND;
    s_dialog_allowed=v->completion_allowed || s_dialog_discard;
    s_dialog=1; s_choice=0;
    s_modal=lv_obj_create(s_screen); lv_obj_set_size(s_modal,224,224);
    lv_obj_align(s_modal,LV_ALIGN_TOP_MID,0,58);
    lv_obj_set_style_bg_color(s_modal,lv_color_hex(CARD),0);
    lv_obj_set_style_border_width(s_modal,0,0); lv_obj_set_style_pad_all(s_modal,10,0);
    lv_obj_remove_flag(s_modal,LV_OBJ_FLAG_SCROLLABLE);
    label(s_modal,"待办操作",INK);
    lv_obj_t *title=content_label(s_modal,v->titles[(s_selected-1)%3],INK);
    lv_obj_set_size(title,198,35); lv_obj_align(title,LV_ALIGN_TOP_LEFT,0,24);
    lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    s_modal_hint=label(s_modal,!v->completion_configured ? "请先配置独立完成令牌" :
        !v->journal_ready ? "完成记录异常，仅可浏览" : v->operation!=TASK_OP_NONE ? operation_name(v->operation) :
        v->pending_count>=TASK_OP_CAPACITY ? "完成队列已满，请先同步" :
        !v->completion_allowed ? "请刷新，或该待办为只读" : "选择操作，长按返回列表",DIM);
    lv_obj_set_width(s_modal_hint,198); lv_label_set_long_mode(s_modal_hint,LV_LABEL_LONG_WRAP);
    lv_obj_align(s_modal_hint,LV_ALIGN_TOP_LEFT,0,62);
    for (unsigned i=0;i<2;i++) {
        s_choices[i]=lv_obj_create(s_modal); lv_obj_set_size(s_choices[i],198,34);
        lv_obj_align(s_choices[i],LV_ALIGN_TOP_LEFT,0,122+(int)i*40);
        lv_obj_set_style_border_width(s_choices[i],0,0); lv_obj_set_style_pad_all(s_choices[i],4,0);
        lv_obj_remove_flag(s_choices[i],LV_OBJ_FLAG_SCROLLABLE);
        s_choice_labels[i]=label(s_choices[i],"",INK);
    }
    paint_dialog();
}
static void detail_action(suite_action_t a)
{
    if (!bsp_lvgl_lock(100)) return;
    if (s_dialog) {
        bool back=false,submit=false,discard=s_dialog_discard;
        if (a==SUITE_ACTION_UP || a==SUITE_ACTION_DOWN) { s_choice^=1U; paint_dialog(); }
        else if (a==SUITE_ACTION_OK) {
            if (s_dialog==1) {
                if (!s_choice) back=true;
                else if (s_dialog_allowed) { s_dialog=2; s_choice=0; paint_dialog(); }
            } else if (!s_choice) {
                lv_obj_delete(s_modal); s_modal=NULL; s_dialog=0;
            } else submit=true;
        }
        bsp_lvgl_unlock();
        if (submit) {
            bool accepted=discard ? suite_tasks_discard_conflict(s_dialog_id) : suite_tasks_complete(s_dialog_id,s_dialog_etag);
            if (accepted) (void)suite_app_back();
            else if (bsp_lvgl_lock(100)) { lv_label_set_text(s_modal_hint,"操作繁忙，请稍后重试"); bsp_lvgl_unlock(); }
        } else if (back) (void)suite_app_back();
        return;
    }
    if (a==SUITE_ACTION_OK) {
        const suite_tasks_view_t *v=suite_tasks_view_acquire();
        if (v && s_selected && v->selected==s_selected-1) open_dialog(v);
        if (v) suite_tasks_view_release();
        bsp_lvgl_unlock(); return;
    }
    bool change=false;
    if (a==SUITE_ACTION_DOWN) {
        if (lv_obj_get_scroll_bottom(s_body)>0) lv_obj_scroll_by(s_body,0,-64,LV_ANIM_OFF);
        else if (s_note+1<s_note_pages) { s_note++; change=true; }
    } else if (a==SUITE_ACTION_UP) {
        if (lv_obj_get_scroll_y(s_body)>0) lv_obj_scroll_by(s_body,0,64,LV_ANIM_OFF);
        else if (s_note) { s_note--; change=true; }
    }
    bsp_lvgl_unlock();
    if (change) suite_tasks_select(s_selected-1,true,s_note);
}
const suite_page_t SUITE_PAGE_TASKS={.name="tasks",.enter=list_enter,.exit=exit_page,.action=list_action};
const suite_page_t SUITE_PAGE_TASK_DETAIL={.name="task_detail",.enter=detail_enter,.exit=exit_page,.action=detail_action};

static bool s_voice_confirm;
static task_voice_state_t s_voice_visible;
static task_op_state_t s_voice_op;
static unsigned s_voice_choice;
static void voice_tick(lv_timer_t *t)
{
    (void)t;
    const suite_tasks_view_t *v=suite_tasks_view_acquire(); if (!v) return;
    s_voice_visible=v->voice_state; s_voice_op=v->voice_operation;
    lv_label_set_text(s_status,v->voice_message);
    if (!s_loaded || s_serial!=v->serial) {
        char text[860];
        snprintf(text,sizeof(text),"%.256s\n%.512s\n%.64s",v->voice_collection[0] ? v->voice_collection : "当前已选 Tasks 列表",
            v->voice_summary[0] ? v->voice_summary : "按OK录制任务标题，最长12秒。\n结束录音后，先核对文字，再确认创建。", 
            v->voice_state==VOICE_RECORDING ? "正在录音" : "");
        lv_label_set_text(s_receipt,text);
        s_loaded=true; s_serial=v->serial;
    }
    char hint[100];
    if (s_voice_confirm) snprintf(hint,sizeof(hint),
        s_voice_op==TASK_OP_CONFLICT || s_voice_op==TASK_OP_READONLY || s_voice_op==TASK_OP_NOT_FOUND ?
        "%s取消  %s放弃请求" : "%s取消  %s确认创建",s_voice_choice ? "" : ">",s_voice_choice ? ">" : "");
    else if (s_voice_visible==VOICE_RECORDING) snprintf(hint,sizeof(hint),"已录%u秒  OK结束 长按取消",v->voice_seconds);
    else snprintf(hint,sizeof(hint),"上下滚动 OK操作 长按返回");
    lv_label_set_text(s_hint,hint);
    suite_ui_battery_update(s_battery); suite_tasks_view_release();
}
static void voice_enter(void)
{
    s_detail=false; s_voice_confirm=false; s_voice_choice=0; s_loaded=false; s_serial=0;
    s_screen=lv_obj_create(NULL); lv_obj_set_style_bg_color(s_screen,lv_color_hex(BG),0);
    lv_obj_remove_flag(s_screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title=label(s_screen,"语音新建待办",INK); lv_obj_align(title,LV_ALIGN_TOP_LEFT,12,8);
    s_battery=suite_ui_battery_create(s_screen);
    s_status=label(s_screen,"正在准备",DIM); lv_obj_align(s_status,LV_ALIGN_TOP_LEFT,12,34);
    lv_obj_set_width(s_status,216); lv_label_set_long_mode(s_status,LV_LABEL_LONG_WRAP);
    s_body=lv_obj_create(s_screen); lv_obj_set_size(s_body,224,192); lv_obj_align(s_body,LV_ALIGN_TOP_MID,0,82);
    lv_obj_set_style_border_width(s_body,0,0); lv_obj_set_style_pad_all(s_body,8,0);
    lv_obj_set_style_bg_color(s_body,lv_color_hex(CARD),0); lv_obj_set_scroll_dir(s_body,LV_DIR_VER);
    s_receipt=content_label(s_body,"",INK); lv_obj_set_width(s_receipt,202); lv_label_set_long_mode(s_receipt,LV_LABEL_LONG_WRAP);
    s_hint=label(s_screen,"上下滚动 OK操作 长按返回",DIM); lv_obj_set_width(s_hint,216);
    lv_label_set_long_mode(s_hint,LV_LABEL_LONG_WRAP); lv_obj_align(s_hint,LV_ALIGN_TOP_LEFT,12,280);
    s_timer=lv_timer_create(voice_tick,250,NULL); lv_screen_load(s_screen); voice_tick(NULL);
}
static void voice_exit(void)
{
    /* Only volatile capture/preview is cancelled. Confirmed durable work stays. */
    (void)suite_tasks_voice(4); exit_page();
}
static void voice_action(suite_action_t a)
{
    if (!bsp_lvgl_lock(100)) return;
    unsigned command=0;
    if (a==SUITE_ACTION_UP || a==SUITE_ACTION_DOWN) {
        if (s_voice_confirm) s_voice_choice^=1U;
        else lv_obj_scroll_by(s_body,0,a==SUITE_ACTION_UP ? 64 : -64,LV_ANIM_OFF);
    } else if (a==SUITE_ACTION_OK) {
        if (s_voice_confirm) {
            if (s_voice_choice) command=s_voice_visible==VOICE_PREVIEW ? 3 : 6;
            s_voice_confirm=false;
        } else if (s_voice_visible==VOICE_RECORDING) command=2;
        else if (s_voice_visible==VOICE_READY || s_voice_visible==VOICE_FAILED) command=1;
        else if (s_voice_visible==VOICE_PREVIEW) { s_voice_confirm=true; s_voice_choice=0; }
        else if (s_voice_visible==VOICE_SUCCEEDED) command=6;
        else if (s_voice_visible==VOICE_PENDING) {
            if (s_voice_op==TASK_OP_CONFLICT || s_voice_op==TASK_OP_READONLY || s_voice_op==TASK_OP_NOT_FOUND) {
                s_voice_confirm=true; s_voice_choice=0;
            } else command=5;
        }
    }
    voice_tick(NULL); bsp_lvgl_unlock();
    if (command) (void)suite_tasks_voice(command);
}
const suite_page_t SUITE_PAGE_TASK_CREATE={.name="task_create",.enter=voice_enter,.exit=voice_exit,.action=voice_action};
