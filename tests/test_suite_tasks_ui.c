/* Exercise the real task pages, not a copy of their input dispatch. */
#include "../main/suite_tasks_ui.c"
#include <assert.h>
#include <stdlib.h>

const lv_font_t suite_font_16={.line_height=20};
const lv_font_t suite_tasks_font_16={.line_height=31};
static suite_tasks_view_t view;
static const suite_page_t *page;
static unsigned refreshes,cancels,opens,borrows,objects,timers,completions,discards;
static bool locked;
static unsigned voice_commands[7];
bool suite_tasks_voice(unsigned cmd) { voice_commands[cmd]++; return true; }

bool bsp_lvgl_lock(unsigned timeout) { (void)timeout; assert(!locked); locked=true; return true; }
void bsp_lvgl_unlock(void) { assert(locked); locked=false; }
const suite_tasks_view_t *suite_tasks_view_acquire(void) { assert(!borrows); borrows++; return &view; }
void suite_tasks_view_release(void) { assert(borrows==1); borrows--; }
suite_conn_state_t suite_connection_state(void) { return SUITE_CONN_ONLINE; }
void suite_tasks_select(unsigned index,bool detail,unsigned note)
{
    view.selected=index<view.count ? index : view.count ? view.count-1 : 0;
    view.detail=detail; view.note_page=note; view.note_pages=2; view.serial++;
}
void suite_tasks_refresh(void) { refreshes++; view.state=TASKS_REFRESHING; }
void suite_tasks_cancel(void) { cancels++; view.state=TASKS_CANCELLED; }
bool suite_tasks_complete(const char *id,const char *etag)
{ assert(strlen(id)==64 && id[0]=='1' && !strcmp(etag,"\"base\"")); completions++; return true; }
bool suite_tasks_discard_conflict(const char *id)
{ assert(strlen(id)==64 && id[0]=='1'); discards++; return true; }
bool suite_app_push(const suite_page_t *next)
{
    assert(!locked && (next==&SUITE_PAGE_TASK_DETAIL || next==&SUITE_PAGE_TASK_CREATE)); opens++;
    page->exit(); page=next; page->enter(); return true;
}
bool suite_app_back(void)
{
    assert(!locked && (page==&SUITE_PAGE_TASK_DETAIL || page==&SUITE_PAGE_TASK_CREATE));
    page->exit(); page=&SUITE_PAGE_TASKS; page->enter(); return true;
}
lv_obj_t *suite_ui_battery_create(lv_obj_t *parent) { return lv_label_create(parent); }
void suite_ui_battery_update(lv_obj_t *o) { (void)o; }

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    lv_obj_t *o=calloc(1,sizeof(*o)); assert(o); objects++;
    o->parent=parent; o->width=240; o->height=320;
    if (parent) { assert(parent->count<16); parent->children[parent->count++]=o; }
    return o;
}
lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int state)
{ (void)state; o->font=f; o->height=f->line_height; }
void lv_label_set_text(lv_obj_t *o,const char *text)
{ assert(strlen(text)<sizeof(o->text)); strcpy(o->text,text); }
void lv_obj_set_style_text_color(lv_obj_t *o,lv_color_t c,int s) { (void)o; (void)c; (void)s; }
void lv_obj_set_style_bg_color(lv_obj_t *o,lv_color_t c,int s) { (void)s; o->bg=c; }
void lv_obj_clean(lv_obj_t *o) { while (o->count) lv_obj_delete(o->children[--o->count]); }
void lv_obj_delete(lv_obj_t *o)
{
    lv_obj_clean(o);
    if (o->parent) for (unsigned i=0;i<o->parent->count;i++) if (o->parent->children[i]==o) {
        memmove(o->parent->children+i,o->parent->children+i+1,(o->parent->count-i-1)*sizeof(o));
        o->parent->count--; break;
    }
    free(o); objects--;
}
void lv_obj_set_width(lv_obj_t *o,int w) { o->width=w; }
void lv_obj_set_size(lv_obj_t *o,int w,int h) { o->width=w; o->height=h; }
void lv_label_set_long_mode(lv_obj_t *o,int mode) { (void)o; (void)mode; }
void lv_obj_align(lv_obj_t *o,int align,int x,int y)
{
    o->x=x; o->y=align==LV_ALIGN_BOTTOM_LEFT ? 320-o->height+y : y;
}
void lv_obj_remove_flag(lv_obj_t *o,int flag) { o->flags&=~(unsigned)flag; }
void lv_obj_add_flag(lv_obj_t *o,int flag) { o->flags|=(unsigned)flag; }
void lv_obj_set_style_border_width(lv_obj_t *o,int width,int state) { (void)o; (void)width; (void)state; }
void lv_obj_set_style_pad_all(lv_obj_t *o,int pad,int state) { (void)state; o->pad=pad; }
void lv_obj_set_flex_flow(lv_obj_t *o,int flow) { (void)o; (void)flow; }
void lv_obj_set_scroll_dir(lv_obj_t *o,int dir) { (void)o; (void)dir; }
void lv_obj_scroll_to_y(lv_obj_t *o,int y,int anim) { (void)anim; o->scroll=y; }
void lv_obj_scroll_by(lv_obj_t *o,int x,int y,int anim)
{ (void)x; (void)anim; o->scroll-=y; o->bottom+=y; }
int lv_obj_get_scroll_y(lv_obj_t *o) { return o->scroll; }
int lv_obj_get_scroll_bottom(lv_obj_t *o) { return o->bottom; }
lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *),unsigned ms,void *data)
{
    (void)ms; (void)data; lv_timer_t *t=malloc(sizeof(*t)); assert(t); timers++; t->callback=callback; return t;
}
void lv_timer_delete(lv_timer_t *t) { free(t); timers--; }
void lv_screen_load(lv_obj_t *o) { (void)o; }

static void draw(void) { assert(s_timer); s_timer->callback(s_timer); assert(!borrows); }
static void act(suite_action_t action) { page->action(action); assert(!locked); draw(); }
static void enter(unsigned count)
{
    memset(&view,0,sizeof(view)); view.count=count; view.cached=true; view.state=TASKS_READY;
    view.server_at=1792368000; view.device_at=1792368010;
    view.completion_configured=true; view.journal_ready=true; view.completion_allowed=true;
    memset(view.task_id,'1',64); view.task_id[64]=0; strcpy(view.etag,"\"base\"");
    strcpy(view.status,"IN-PROCESS"); strcpy(view.due,"无到期时间");
    strcpy(view.note,"中文备注"); strcpy(view.titles[0],"中文待办");
    strcpy(view.titles[1],"第二条"); strcpy(view.titles[2],"第三条");
    page=&SUITE_PAGE_TASKS; page->enter(); draw();
}
static void leave(void) { page->exit(); assert(!objects && !timers && !borrows); }

int main(void)
{
    const unsigned counts[]={0,1,2,4,5,6,7,154,200,208};
    for (unsigned n=0;n<sizeof(counts)/sizeof(counts[0]);n++) {
        unsigned count=counts[n],slot=0;
        /* Both directions visit Refresh and every task exactly once per cycle. */
        for (unsigned i=0;i<=count;i++) { assert(slot==i); slot=task_list_move(slot,count,true); }
        assert(!slot);
        for (unsigned i=0;i<=count;i++) slot=task_list_move(slot,count,false);
        assert(!slot);
        enter(count);
        for (unsigned i=0;i<3;i++) assert(!!(s_rows[i]->flags&LV_OBJ_FLAG_HIDDEN)==(i>=count));
        assert(!s_selected && s_refresh_row->bg==SELECT);
        assert(!strcmp(s_refresh_label->text,"刷新待办"));
        assert(!strcmp(s_voice_label->text,"语音新建"));
        assert(s_refresh_row->x+s_refresh_row->width<=s_voice_row->x);
        assert(s_refresh_row->y+s_refresh_row->height<=s_rows[0]->y);
        assert(s_rows[2]->y+s_rows[2]->height<=s_receipt->y);
        assert(s_refresh_label->font->line_height+2*s_refresh_row->pad<=s_refresh_row->height);
        assert(s_receipt->y+2*s_receipt->font->line_height<=s_hint->y);
        assert(s_refresh_label->font==&suite_font_16 && s_status->font==&suite_font_16);
        assert(s_labels[0]->font==&suite_tasks_font_16);
        unsigned before=refreshes;
        act(SUITE_ACTION_OK); assert(refreshes==before+1 && !opens);
        act(SUITE_ACTION_OK); assert(cancels==n+1 && !opens);
        view.state=TASKS_READY; draw();
        act(SUITE_ACTION_UP); assert(s_selected==count+1 && s_voice_row->bg==SELECT);
        if (count) { act(SUITE_ACTION_UP); assert(s_selected==count); }
        unsigned first=count ? (count-1)/3*3 : 0;
        for (unsigned i=0;i<3;i++) assert(!!(s_rows[i]->flags&LV_OBJ_FLAG_HIDDEN)==(first+i>=count));
        if (count) act(SUITE_ACTION_DOWN);
        act(SUITE_ACTION_DOWN); assert(!s_selected);
        leave();
    }
    enter(154); act(SUITE_ACTION_DOWN); assert(s_selected==1 && view.selected==0);
    act(SUITE_ACTION_OK); assert(opens==1 && page==&SUITE_PAGE_TASK_DETAIL);
    assert(strstr(s_body->children[1]->text,"进行中"));
    assert(strstr(s_receipt->text,"OK操作"));
    s_body->bottom=64; act(SUITE_ACTION_DOWN); assert(s_body->scroll==64);
    act(SUITE_ACTION_DOWN); assert(view.note_page==1);
    act(SUITE_ACTION_OK); assert(s_dialog==1 && !s_choice);
    act(SUITE_ACTION_OK); assert(page==&SUITE_PAGE_TASKS && s_selected==1);
    act(SUITE_ACTION_UP); assert(!s_selected && s_refresh_row->bg==SELECT);
    view.count=6; draw(); assert(!s_selected && strstr(s_status->text,"6条"));
    view.state=TASKS_FAILED; draw(); assert(!strcmp(s_status->text,"刷新失败，保留缓存"));
    leave();
    enter(6); assert(!s_selected);
    act(SUITE_ACTION_DOWN);
    /* The worker can replace a cache before the next 250 ms paint. */
    view.count=0;
    unsigned before=refreshes;
    act(SUITE_ACTION_OK); assert(refreshes==before+1 && opens==1);
    leave();
    enter(0); view.state=TASKS_LOADING; view.cached=false; draw();
    view.count=6; view.state=TASKS_READY; view.cached=true; draw();
    assert(!s_selected);
    before=refreshes; act(SUITE_ACTION_OK); assert(refreshes==before+1);
    leave();
    enter(2); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    act(SUITE_ACTION_OK); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    assert(s_dialog==2 && !s_choice && !completions);
    act(SUITE_ACTION_OK); assert(!s_dialog && page==&SUITE_PAGE_TASK_DETAIL);
    act(SUITE_ACTION_OK); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    /* A changed view cannot substitute a different identity in confirmation. */
    memset(view.task_id,'2',64);
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    assert(completions==1 && page==&SUITE_PAGE_TASKS); leave();
    enter(2); view.completion_allowed=false; view.completion_configured=false;
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); act(SUITE_ACTION_OK);
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); assert(s_dialog==1 && completions==1);
    act(SUITE_ACTION_UP); act(SUITE_ACTION_OK); leave();
    enter(2); view.completion_allowed=false; view.operation=TASK_OP_CONFLICT;
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); act(SUITE_ACTION_OK);
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); assert(s_dialog==2 && !discards);
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); assert(discards==1); leave();
    enter(1); view.retained_count=1; view.request_only=true;
    view.operation=TASK_OP_NOT_FOUND; view.completion_allowed=false;
    strcpy(view.titles[0],"保留的完成请求"); draw();
    assert(view.retained_count==1 && strstr(s_status->text,"1条"));
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    assert(!strcmp(s_body->children[1]->text,"保留请求"));
    act(SUITE_ACTION_OK); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK);
    assert(s_dialog==2 && !s_choice && discards==1);
    act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); assert(discards==2); leave();
    enter(0); act(SUITE_ACTION_UP); act(SUITE_ACTION_OK);
    assert(page==&SUITE_PAGE_TASK_CREATE);
    view.voice_state=VOICE_PREVIEW; strcpy(view.voice_summary,"语音测试标题"); view.serial++; draw();
    act(SUITE_ACTION_OK); assert(s_voice_confirm && !s_voice_choice);
    act(SUITE_ACTION_OK); assert(!s_voice_confirm && !voice_commands[3]);
    act(SUITE_ACTION_OK); act(SUITE_ACTION_DOWN); act(SUITE_ACTION_OK); assert(voice_commands[3]==1);
    view.voice_state=VOICE_RECORDING; draw(); act(SUITE_ACTION_OK); assert(voice_commands[2]==1);
    unsigned old_cancel=voice_commands[4]; leave(); assert(voice_commands[4]==old_cancel+1);
    puts("task page Refresh / populated lists / Chinese labels / details / lifecycle: PASS");
    return 0;
}
