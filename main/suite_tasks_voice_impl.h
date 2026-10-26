/* Included only by suite_tasks.c: the existing worker owns cache/audio/NVS. */
#include "bsp_audio.h"

static atomic_uint s_voice_command;
static atomic_bool s_voice_stop,s_voice_cancel;
static task_creation_t s_creation;
static task_voice_state_t s_voice_state;
static char s_voice_title[513],s_voice_collection[65],s_voice_name[257],s_voice_message[96];
static unsigned s_voice_seconds,s_voice_attempts;
static int64_t s_voice_due;
static bool s_voice_journal_ready,s_voice_retry;

/* Versioned numeric-only diagnostics for one accepted recording attempt.
   Timestamps are boot-local microseconds. Shared fields are guarded by a
   short critical section: input records only a timestamp and never allocates,
   logs, walks heaps or waits on the worker. A stage that was not reached keeps
   its valid bit clear (logged as -1), not zero. The ESP-IDF 5.5.3 RISC-V port
   defines StackType_t as uint8_t, so uxTaskGetStackHighWaterMark() returns
   bytes; the log field is named stack_b to state that unit explicitly. */
#define VOICE_DIAG_VERSION 2U
#define VOICE_DIAG_MAX_LINES 16U
#define VOICE_DIAG_BIT(n) (1U<<(n))
enum { VOICE_DIAG_T_ACCEPT=0,VOICE_DIAG_T_WORKER,VOICE_DIAG_T_PREFLIGHT_START,
       VOICE_DIAG_T_PREFLIGHT_DONE,VOICE_DIAG_T_CAPTURE_START,VOICE_DIAG_T_CAPTURE_END,
       VOICE_DIAG_T_RELEASE_START,VOICE_DIAG_T_RELEASE_DONE,VOICE_DIAG_T_UPLOAD_START,
       VOICE_DIAG_T_UPLOAD_DONE,VOICE_DIAG_T_CLEANUP_START,VOICE_DIAG_T_CLEANUP_DONE,
       VOICE_DIAG_T_STOP_REQ,VOICE_DIAG_T_CANCEL_REQ,VOICE_DIAG_T_FINAL,
       VOICE_DIAG_T_UPLOAD_OPEN,VOICE_DIAG_T_UPLOAD_SENT,VOICE_DIAG_T_UPLOAD_RESP };
enum { VOICE_DIAG_STAGE_ACCEPTED=1,VOICE_DIAG_STAGE_WORKER_START,
       VOICE_DIAG_STAGE_PREFLIGHT_START,VOICE_DIAG_STAGE_PREFLIGHT_ALIVE,
       VOICE_DIAG_STAGE_PREFLIGHT_DONE,VOICE_DIAG_STAGE_CAPTURE_START,
       VOICE_DIAG_STAGE_CAPTURE_END,VOICE_DIAG_STAGE_RELEASE_START,
       VOICE_DIAG_STAGE_RELEASE_DONE,VOICE_DIAG_STAGE_UPLOAD_START,
       VOICE_DIAG_STAGE_UPLOAD_ALIVE,VOICE_DIAG_STAGE_UPLOAD_DONE,
       VOICE_DIAG_STAGE_CLEANUP_START,VOICE_DIAG_STAGE_CLEANUP_DONE,
       VOICE_DIAG_STAGE_FINAL };
enum { VOICE_DIAG_RESULT_NONE=0,VOICE_DIAG_RESULT_PREVIEW=1,
       VOICE_DIAG_RESULT_CANCEL_PREWORKER=2,VOICE_DIAG_RESULT_CANCEL_PREFLIGHT=3,
       VOICE_DIAG_RESULT_CANCEL_CAPTURE=4,VOICE_DIAG_RESULT_CANCEL_UPLOAD=5,
       VOICE_DIAG_RESULT_CANCEL_CLEANUP=6,VOICE_DIAG_RESULT_UNREADY=7,
       VOICE_DIAG_RESULT_INTENT_PENDING=8,VOICE_DIAG_RESULT_OFFLINE=9,
       VOICE_DIAG_RESULT_AUTH=10,VOICE_DIAG_RESULT_ADMISSION=11,
       VOICE_DIAG_RESULT_ALLOC=12,VOICE_DIAG_RESULT_NETWORK=13,
       VOICE_DIAG_RESULT_AUDIO=14,VOICE_DIAG_RESULT_STORAGE=15,
       VOICE_DIAG_RESULT_TRANSPORT=16,VOICE_DIAG_RESULT_CLEANUP_FAILED=17,
       VOICE_DIAG_RESULT_INTERNAL=18,VOICE_DIAG_RESULT_BUDGET=19 };
enum { VOICE_DIAG_CAP_NONE=0,VOICE_DIAG_CAP_BYTES_FULL=1,VOICE_DIAG_CAP_STOP=2,
       VOICE_DIAG_CAP_CANCEL=3,VOICE_DIAG_CAP_DEADLINE=4,VOICE_DIAG_CAP_AUDIO_FAIL=5,
       VOICE_DIAG_CAP_FLASH_FAIL=6,VOICE_DIAG_CAP_RELEASE_FAIL=7,
       VOICE_DIAG_CAP_SHORT=8 };
enum { VOICE_DIAG_FLAG_STOP_REQ=1U<<0,VOICE_DIAG_FLAG_CANCEL_REQ=1U<<1,
       VOICE_DIAG_FLAG_CLEANUP_FAILED=1U<<2,VOICE_DIAG_FLAG_CACHE_BLOCKED=1U<<3,
       VOICE_DIAG_FLAG_AUDIO_STARTED=1U<<4,VOICE_DIAG_FLAG_RELEASE_FAILED=1U<<5,
       VOICE_DIAG_FLAG_ACTIVE=1U<<30,VOICE_DIAG_FLAG_FINALIZED=1U<<31 };
typedef struct {
    uint32_t seq,valid,stage,result,capture,flags,bytes,line_count;
    int status;
    int64_t t_accept,t_worker,t_pf_start,t_pf_done,t_cap_start,t_cap_end,
            t_rel_start,t_rel_done,t_up_start,t_up_open,t_up_sent,t_up_resp,t_up_done,
            t_cl_start,t_cl_done,t_stop,t_cancel,t_final;
} voice_diag_t;
static voice_diag_t s_voice_diag;
/* A terminal publish can wake a new accepted start before the worker reaches
   voice_diag_finalize(). The single worker cannot produce two such deferred
   records at once, so one fixed pending slot keeps the older sequence without
   allocation or input-context logging. */
static voice_diag_t s_voice_diag_pending;
static uint32_t s_voice_diag_seq;
static portMUX_TYPE s_voice_diag_mux=portMUX_INITIALIZER_UNLOCKED;

/* Caller holds s_voice_diag_mux. Finalized records are never matched. */
static voice_diag_t *voice_diag_lookup(unsigned seq)
{
    if (!seq) return NULL;
    if (s_voice_diag.seq==seq && (s_voice_diag.flags&VOICE_DIAG_FLAG_ACTIVE) &&
        !(s_voice_diag.flags&VOICE_DIAG_FLAG_FINALIZED)) return &s_voice_diag;
    if (s_voice_diag_pending.seq==seq && (s_voice_diag_pending.flags&VOICE_DIAG_FLAG_ACTIVE) &&
        !(s_voice_diag_pending.flags&VOICE_DIAG_FLAG_FINALIZED)) return &s_voice_diag_pending;
    return NULL;
}

static void voice_diag_sample(int64_t *ifree,int64_t *ilarge,int64_t *dfree,
                              int64_t *dlarge,int64_t *stack)
{
    /* Internal and DMA capability sets overlap; never add these values. */
    *ifree=(int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    *ilarge=(int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    *dfree=(int64_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    *dlarge=(int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    *stack=(int64_t)uxTaskGetStackHighWaterMark(NULL);
}
static long long voice_diag_delta(const voice_diag_t *r,int64_t later,uint32_t later_bit,
                                  int64_t earlier,uint32_t earlier_bit)
{
    if (!(r->valid&VOICE_DIAG_BIT(later_bit)) || !(r->valid&VOICE_DIAG_BIT(earlier_bit)))
        return -1;
    return (long long)(later-earlier);
}
static void voice_diag_log_stage(const voice_diag_t *r,unsigned ev,int64_t now,bool rs,
                                 int64_t ifree,int64_t ilarge,int64_t dfree,
                                 int64_t dlarge,int64_t stack)
{
    long long d=(long long)(now-r->t_accept);
    if (rs) ESP_LOGI(TAG,"TVDIAG v=%u seq=%u ev=%u stage=%u valid=0x%x rs=1 t=%lld d=%lld "
        "bytes=%u cap=%u flags=0x%x status=%d ifree_b=%lld ilarge_b=%lld dfree_b=%lld "
        "dlarge_b=%lld stack_b=%lld",VOICE_DIAG_VERSION,r->seq,ev,r->stage,r->valid,
        (long long)now,d,r->bytes,r->capture,r->flags,r->status,
        (long long)ifree,(long long)ilarge,(long long)dfree,(long long)dlarge,(long long)stack);
    else ESP_LOGI(TAG,"TVDIAG v=%u seq=%u ev=%u stage=%u valid=0x%x rs=0 t=%lld d=%lld "
        "bytes=%u cap=%u flags=0x%x status=%d ifree_b=-1 ilarge_b=-1 dfree_b=-1 "
        "dlarge_b=-1 stack_b=-1",VOICE_DIAG_VERSION,r->seq,ev,r->stage,r->valid,
        (long long)now,d,r->bytes,r->capture,r->flags,r->status);
}
static void voice_diag_log_final(const voice_diag_t *r,int64_t ifree,int64_t ilarge,
                                 int64_t dfree,int64_t dlarge,int64_t stack)
{
    long long total=voice_diag_delta(r,r->t_final,VOICE_DIAG_T_FINAL,r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long worker=voice_diag_delta(r,r->t_worker,VOICE_DIAG_T_WORKER,r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long preflight=voice_diag_delta(r,r->t_pf_done,VOICE_DIAG_T_PREFLIGHT_DONE,
                                         r->t_pf_start,VOICE_DIAG_T_PREFLIGHT_START);
    long long capture=voice_diag_delta(r,r->t_cap_end,VOICE_DIAG_T_CAPTURE_END,
                                       r->t_cap_start,VOICE_DIAG_T_CAPTURE_START);
    long long release=voice_diag_delta(r,r->t_rel_done,VOICE_DIAG_T_RELEASE_DONE,
                                       r->t_rel_start,VOICE_DIAG_T_RELEASE_START);
    long long upload=voice_diag_delta(r,r->t_up_done,VOICE_DIAG_T_UPLOAD_DONE,
                                      r->t_up_start,VOICE_DIAG_T_UPLOAD_START);
    long long up_open=voice_diag_delta(r,r->t_up_open,VOICE_DIAG_T_UPLOAD_OPEN,
                                       r->t_up_start,VOICE_DIAG_T_UPLOAD_START);
    long long up_send=voice_diag_delta(r,r->t_up_sent,VOICE_DIAG_T_UPLOAD_SENT,
                                       r->t_up_open,VOICE_DIAG_T_UPLOAD_OPEN);
    long long up_wait=voice_diag_delta(r,r->t_up_resp,VOICE_DIAG_T_UPLOAD_RESP,
                                       r->t_up_sent,VOICE_DIAG_T_UPLOAD_SENT);
    long long up_tail=voice_diag_delta(r,r->t_up_done,VOICE_DIAG_T_UPLOAD_DONE,
                                       r->t_up_resp,VOICE_DIAG_T_UPLOAD_RESP);
    long long cleanup=voice_diag_delta(r,r->t_cl_done,VOICE_DIAG_T_CLEANUP_DONE,
                                       r->t_cl_start,VOICE_DIAG_T_CLEANUP_START);
    long long stop=voice_diag_delta(r,r->t_stop,VOICE_DIAG_T_STOP_REQ,
                                    r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long stop_edge=voice_diag_delta(r,r->t_cap_end,VOICE_DIAG_T_CAPTURE_END,
                                         r->t_stop,VOICE_DIAG_T_STOP_REQ);
    long long cancel=voice_diag_delta(r,r->t_cancel,VOICE_DIAG_T_CANCEL_REQ,
                                      r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long cancel_edge=voice_diag_delta(r,r->t_cap_end,VOICE_DIAG_T_CAPTURE_END,
                                           r->t_cancel,VOICE_DIAG_T_CANCEL_REQ);
    long long capture_end=voice_diag_delta(r,r->t_cap_end,VOICE_DIAG_T_CAPTURE_END,
                                           r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long release_done=voice_diag_delta(r,r->t_rel_done,VOICE_DIAG_T_RELEASE_DONE,
                                            r->t_accept,VOICE_DIAG_T_ACCEPT);
    long long cleanup_done=voice_diag_delta(r,r->t_cl_done,VOICE_DIAG_T_CLEANUP_DONE,
                                            r->t_accept,VOICE_DIAG_T_ACCEPT);
    ESP_LOGI(TAG,"TVDIAGF v=%u seq=%u result=%u stage=%u valid=0x%x rs=1 t=%lld "
        "d_total=%lld d_worker=%lld d_preflight=%lld d_capture=%lld d_capture_end=%lld "
        "d_release=%lld d_release_done=%lld d_upload=%lld d_up_open=%lld "
        "d_up_send=%lld d_up_wait=%lld d_up_tail=%lld d_cleanup=%lld "
        "d_cleanup_done=%lld d_stop=%lld d_stop_edge=%lld d_cancel=%lld "
        "d_cancel_edge=%lld bytes=%u cap=%u flags=0x%x status=%d ifree_b=%lld "
        "ilarge_b=%lld dfree_b=%lld dlarge_b=%lld stack_b=%lld",
        VOICE_DIAG_VERSION,r->seq,r->result,r->stage,r->valid,(long long)r->t_final,
        total,worker,preflight,capture,capture_end,release,release_done,upload,
        up_open,up_send,up_wait,up_tail,cleanup,cleanup_done,stop,stop_edge,cancel,
        cancel_edge,r->bytes,r->capture,r->flags,r->status,(long long)ifree,
        (long long)ilarge,(long long)dfree,(long long)dlarge,(long long)stack);
}
static bool voice_diag_mark(unsigned seq,unsigned stage,bool emit,bool rs)
{
    /* The boundary timestamp is taken before resource sampling so the log
       does not charge observer work to the interval that just ended. */
    int64_t now=esp_timer_get_time();
    int64_t ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
    if (rs) voice_diag_sample(&ifree,&ilarge,&dfree,&dlarge,&stack);
    voice_diag_t snap; bool matched=false,log=false;
    portENTER_CRITICAL(&s_voice_diag_mux);
    voice_diag_t *r=voice_diag_lookup(seq);
    if (r) {
        matched=true;
        switch (stage) {
        case VOICE_DIAG_STAGE_WORKER_START:
            r->t_worker=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_WORKER); break;
        case VOICE_DIAG_STAGE_PREFLIGHT_START:
            r->t_pf_start=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_PREFLIGHT_START); break;
        case VOICE_DIAG_STAGE_PREFLIGHT_DONE:
            r->t_pf_done=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_PREFLIGHT_DONE); break;
        case VOICE_DIAG_STAGE_CAPTURE_START:
            r->t_cap_start=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_CAPTURE_START); break;
        case VOICE_DIAG_STAGE_CAPTURE_END:
            r->t_cap_end=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_CAPTURE_END); break;
        case VOICE_DIAG_STAGE_RELEASE_START:
            r->t_rel_start=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_RELEASE_START); break;
        case VOICE_DIAG_STAGE_RELEASE_DONE:
            r->t_rel_done=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_RELEASE_DONE); break;
        case VOICE_DIAG_STAGE_UPLOAD_START:
            r->t_up_start=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_UPLOAD_START); break;
        case VOICE_DIAG_STAGE_UPLOAD_DONE:
            r->t_up_done=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_UPLOAD_DONE); break;
        case VOICE_DIAG_STAGE_CLEANUP_START:
            r->t_cl_start=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_CLEANUP_START); break;
        case VOICE_DIAG_STAGE_CLEANUP_DONE:
            r->t_cl_done=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_CLEANUP_DONE); break;
        default: break; /* ALIVE checkpoints only sample resources. */
        }
        r->stage=stage;
        if (emit && r->line_count<VOICE_DIAG_MAX_LINES-1) {
            r->line_count++; log=true;
        }
        snap=*r;
    }
    portEXIT_CRITICAL(&s_voice_diag_mux);
    if (log) voice_diag_log_stage(&snap,stage==VOICE_DIAG_STAGE_WORKER_START ? 1U : 2U,
        now,rs,ifree,ilarge,dfree,dlarge,stack);
    return matched;
}
static void voice_diag_accept(void)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_voice_diag_mux);
    /* Preserve an attempt that published a terminal state but has not reached
       finalize yet. The single worker guarantees this slot is free before a
       later worker attempt can publish, so no allocation or queue is needed. */
    if (s_voice_diag.seq && (s_voice_diag.flags&VOICE_DIAG_FLAG_ACTIVE) &&
        !(s_voice_diag.flags&VOICE_DIAG_FLAG_FINALIZED) && !s_voice_diag_pending.seq)
        s_voice_diag_pending=s_voice_diag;
    uint32_t seq=++s_voice_diag_seq;
    if (!seq) seq=++s_voice_diag_seq;
    memset(&s_voice_diag,0,sizeof(s_voice_diag));
    s_voice_diag.seq=seq; s_voice_diag.flags=VOICE_DIAG_FLAG_ACTIVE;
    s_voice_diag.status=-1; s_voice_diag.t_accept=now;
    s_voice_diag.valid=VOICE_DIAG_BIT(VOICE_DIAG_T_ACCEPT);
    s_voice_diag.stage=VOICE_DIAG_STAGE_ACCEPTED;
    portEXIT_CRITICAL(&s_voice_diag_mux);
}
static void voice_diag_request(bool cancel)
{
    /* Publish the business flag and record its timestamp in one bounded
       critical operation. A permitted switch can otherwise process the cancel
       and finalize the attempt before the request is captured. The pinned
       ESP-IDF 5.5.3 esp_timer_get_time is a bounded systimer register read and
       is safe inside this section. No allocation, logging, heap walk or wait. */
    portENTER_CRITICAL(&s_voice_diag_mux);
    if (cancel) atomic_store(&s_voice_cancel,true);
    else atomic_store(&s_voice_stop,true);
    int64_t now=esp_timer_get_time();
    voice_diag_t *r=NULL;
    if (s_voice_diag.seq && (s_voice_diag.flags&VOICE_DIAG_FLAG_ACTIVE) &&
        !(s_voice_diag.flags&VOICE_DIAG_FLAG_FINALIZED)) r=&s_voice_diag;
    if (r) {
        if (cancel) {
            if (!(r->valid&VOICE_DIAG_BIT(VOICE_DIAG_T_CANCEL_REQ)))
                r->t_cancel=now;
            r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_CANCEL_REQ);
            r->flags|=VOICE_DIAG_FLAG_CANCEL_REQ;
        } else {
            if (!(r->valid&VOICE_DIAG_BIT(VOICE_DIAG_T_STOP_REQ)))
                r->t_stop=now;
            r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_STOP_REQ);
            r->flags|=VOICE_DIAG_FLAG_STOP_REQ;
        }
    }
    portEXIT_CRITICAL(&s_voice_diag_mux);
}
static unsigned voice_diag_worker_start(void)
{
    unsigned seq=0;
    portENTER_CRITICAL(&s_voice_diag_mux);
    if ((s_voice_diag.flags&VOICE_DIAG_FLAG_ACTIVE) &&
        !(s_voice_diag.flags&VOICE_DIAG_FLAG_FINALIZED)) seq=s_voice_diag.seq;
    portEXIT_CRITICAL(&s_voice_diag_mux);
    if (seq) voice_diag_mark(seq,VOICE_DIAG_STAGE_WORKER_START,true,false);
    return seq;
}
static void voice_diag_status(unsigned seq,int status)
{
    portENTER_CRITICAL(&s_voice_diag_mux);
    voice_diag_t *r=voice_diag_lookup(seq);
    if (r) r->status=status;
    portEXIT_CRITICAL(&s_voice_diag_mux);
}
static void voice_diag_capture(unsigned seq,uint32_t bytes,unsigned reason)
{
    portENTER_CRITICAL(&s_voice_diag_mux);
    voice_diag_t *r=voice_diag_lookup(seq);
    if (r) { r->bytes=bytes; r->capture=reason; }
    portEXIT_CRITICAL(&s_voice_diag_mux);
}
static void voice_diag_upload_phase(unsigned seq,unsigned phase)
{
    if (!seq) return;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_voice_diag_mux);
    voice_diag_t *r=voice_diag_lookup(seq);
    if (r) {
        if (phase==1U) { r->t_up_open=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_UPLOAD_OPEN); }
        else if (phase==2U) { r->t_up_sent=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_UPLOAD_SENT); }
        else if (phase==3U) { r->t_up_resp=now; r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_UPLOAD_RESP); }
    }
    portEXIT_CRITICAL(&s_voice_diag_mux);
}
static void voice_diag_http_alive(unsigned seq,unsigned kind,int status)
{
    if (!seq || !kind) return;
    voice_diag_status(seq,status);
    voice_diag_mark(seq,kind==1U ? VOICE_DIAG_STAGE_PREFLIGHT_ALIVE :
                    VOICE_DIAG_STAGE_UPLOAD_ALIVE,true,true);
}
static void voice_diag_finalize(unsigned seq,unsigned result,unsigned capture,
                                unsigned flags,uint32_t bytes,int status)
{
    if (!seq) return;
    /* Timestamp the worker boundary before sampling resources. */
    int64_t now=esp_timer_get_time();
    int64_t ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
    voice_diag_sample(&ifree,&ilarge,&dfree,&dlarge,&stack);
    voice_diag_t snap; bool log=false;
    portENTER_CRITICAL(&s_voice_diag_mux);
    voice_diag_t *r=voice_diag_lookup(seq);
    if (r) {
        r->t_final=now;
        r->valid|=VOICE_DIAG_BIT(VOICE_DIAG_T_FINAL);
        r->stage=VOICE_DIAG_STAGE_FINAL; r->result=result;
        r->capture=capture; r->bytes=bytes; r->status=status;
        r->flags=(r->flags|flags|VOICE_DIAG_FLAG_FINALIZED)&~VOICE_DIAG_FLAG_ACTIVE;
        /* Reserve the last line for the single final result. */
        if (r->line_count<VOICE_DIAG_MAX_LINES) r->line_count++;
        snap=*r; log=true;
        if (r==&s_voice_diag_pending) memset(&s_voice_diag_pending,0,sizeof(s_voice_diag_pending));
    }
    portEXIT_CRITICAL(&s_voice_diag_mux);
    if (log) voice_diag_log_final(&snap,ifree,ilarge,dfree,dlarge,stack);
}

static void voice_publish(task_voice_state_t st,const char *message)
{
    s_voice_state=st;
    snprintf(s_voice_message,sizeof(s_voice_message),"%s",message);
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    s_view.voice_state=st; s_view.voice_operation=(task_op_state_t)s_creation.state;
    s_view.voice_seconds=s_voice_seconds;
    strcpy(s_view.voice_summary,s_creation.magic ? s_creation.summary : s_voice_title);
    strcpy(s_view.voice_collection,s_voice_name);
    strcpy(s_view.voice_message,s_voice_message); s_view.serial++;
    xSemaphoreGive(s_mutex);
}
static bool voice_save(const task_creation_t *candidate)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("taskcfg","taskvoice",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t result=nvs_set_blob(h,"intent",candidate,sizeof(*candidate));
    if (result==ESP_OK) result=nvs_commit(h);
    nvs_close(h); return result==ESP_OK;
}
static void voice_load(void)
{
    s_voice_journal_ready=false;
    s_voice_cache_blocked=suite_audio_job_recover()!=ESP_OK;
    if (s_voice_cache_blocked) { voice_publish(VOICE_STORAGE_FAILED,"录音清理失败，请重启"); return; }
    nvs_handle_t h;
    if (!s_completion_configured || nvs_open_from_partition("taskcfg","taskvoice",NVS_READWRITE,&h)!=ESP_OK) {
        voice_publish(VOICE_FAILED,"请先配置独立待办写令牌"); return;
    }
    size_t bytes=sizeof(s_creation);
    esp_err_t result=nvs_get_blob(h,"intent",&s_creation,&bytes); nvs_close(h);
    if (result!=ESP_ERR_NVS_NOT_FOUND && (result!=ESP_OK || bytes!=sizeof(s_creation) || !task_creation_valid(&s_creation))) {
        memset(&s_creation,0,sizeof(s_creation)); /* Preserve the corrupt NVS blob, never render it. */
        voice_publish(VOICE_STORAGE_FAILED,"新建记录异常，数据保留"); return;
    }
    if (result==ESP_ERR_NVS_NOT_FOUND) memset(&s_creation,0,sizeof(s_creation));
    s_voice_journal_ready=true;
    voice_publish(s_creation.magic ? (s_creation.state==TASK_OP_SUCCEEDED ? VOICE_SUCCEEDED : VOICE_PENDING) : VOICE_READY,
        s_creation.magic ? (s_creation.state==TASK_OP_SUCCEEDED ? "任务已新建，OK继续" : "新建请求已保留，待同步") : "OK开始录音，最长12秒");
}

static const char *voice_error_text(task_voice_error_t error)
{
    if (error==TASK_VOICE_ERROR_AUTH) return "语音认证失败，请检查配置";
    if (error==TASK_VOICE_ERROR_BUDGET) return "语音额度已用完，OK重试";
    if (error==TASK_VOICE_ERROR_TARGET_REQUIRED) return "语音未配置，或目标列表不可写";
    return "语音服务暂时不可用，请稍后重试";
}
/* A missing/corrupt journal or blocked staging area always needs a visible
   terminal cause; callers must not overwrite it with READY. */
static bool voice_publish_unready(void)
{
    if (s_voice_journal_ready && !s_voice_cache_blocked) return false;
    if (s_voice_cache_blocked) voice_publish(VOICE_STORAGE_FAILED,"录音清理失败，请重启");
    else if (!s_completion_configured) voice_publish(VOICE_FAILED,"请先配置独立待办写令牌");
    else voice_publish(VOICE_STORAGE_FAILED,"新建记录异常，数据保留");
    return true;
}
/* Every blocking stage uses the remaining absolute deadline. Handshake and
   non-transcription traffic stay capped at 10 s; only the transcription
   response may wait longer for its first byte. */
static bool voice_stage_timeout(esp_http_client_handle_t client,int64_t deadline,int maximum_ms)
{
    int64_t remain=(deadline-esp_timer_get_time())/1000;
    if (remain<=0) return false;
    if (remain>maximum_ms) remain=maximum_ms;
    return esp_http_client_set_timeout_ms(client,(int)remain)==ESP_OK;
}
/* Caller holds suite_net through preflight/capture/release/upload. Response is
   parsed only after TLS cleanup; audio is read from the inactive Flash slot. */
static bool voice_http(credentials_t *config,const char *path,const char *request,
                       uint32_t audio_base,size_t audio_bytes,char *response,size_t cap,
                       int *status,int response_ms,unsigned diag_seq,unsigned diag_kind)
{
    char url[256],auth[144];
    snprintf(url,sizeof(url),"%s%s",config->origin,path); snprintf(auth,sizeof(auth),"Bearer %s",config->write_token);
    esp_http_client_config_t settings={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=10000,
        .buffer_size=512,.buffer_size_tx=1024,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&settings);
    bool ok=false; size_t used=0; *status=0;
    int64_t deadline=esp_timer_get_time()+45000000;
    if (!client || esp_http_client_set_header(client,"Authorization",auth)!=ESP_OK) goto done;
    size_t bytes=audio_bytes ? audio_bytes : request ? strlen(request) : 0;
    if (bytes && (esp_http_client_set_method(client,HTTP_METHOD_POST)!=ESP_OK ||
        esp_http_client_set_header(client,"Content-Type",audio_bytes ? "audio/wav" : "application/json")!=ESP_OK)) goto done;
    if (!voice_stage_timeout(client,deadline,10000) || esp_http_client_open(client,(int)bytes)!=ESP_OK) goto done;
    if (diag_kind==2U) voice_diag_upload_phase(diag_seq,1U); /* successful open */
    char chunk[512];
    for (size_t sent=0;sent<bytes;) {
        if (esp_timer_get_time()>=deadline || (audio_bytes && atomic_load(&s_voice_cancel))) goto done;
        if (!voice_stage_timeout(client,deadline,10000)) goto done;
        size_t n=bytes-sent; if (n>sizeof(chunk)) n=sizeof(chunk);
        const char *p=request ? request+sent : chunk;
        if (audio_bytes && !io_read((void *)s_partition,audio_base+(uint32_t)sent,chunk,n)) goto done;
        int written=esp_http_client_write(client,p,(int)n);
        if (written<=0) goto done;
        sent+=(size_t)written;
    }
    if (diag_kind==2U) voice_diag_upload_phase(diag_seq,2U); /* completed upload */
    if (!voice_stage_timeout(client,deadline,response_ms)) goto done;
    int64_t length=esp_http_client_fetch_headers(client); *status=esp_http_client_get_status_code(client);
    if (diag_kind==2U) voice_diag_upload_phase(diag_seq,3U); /* response/status observed */
    voice_diag_http_alive(diag_seq,diag_kind,*status); /* TLS/client still alive. */
    /* A late first byte is a timeout, not a successful response. */
    if (esp_timer_get_time()>=deadline || length<0 || length>=(int64_t)cap) goto done;
    while (used<cap-1 && esp_timer_get_time()<deadline) {
        if (audio_bytes && atomic_load(&s_voice_cancel)) goto done;
        if (!voice_stage_timeout(client,deadline,response_ms)) goto done;
        int n=esp_http_client_read(client,response+used,(int)(cap-1-used));
        if (n<0) goto done;
        if (!n) { ok=esp_http_client_is_complete_data_received(client); break; }
        used+=(size_t)n;
    }
    if (esp_timer_get_time()>=deadline) ok=false;
    /* Parsers receive C strings, so a raw NUL inside the actual byte range
       must fail closed before strlen can hide trailing garbage. An incomplete
       body is cleared as well: diagnostics may use status but never stale or
       partial JSON. */
    if (used && memchr(response,0,used)) ok=false;
    if (ok) response[used]=0;
    else response[0]=0;
done:
    if (!ok && response && cap) response[0]=0;
    if (client) esp_http_client_cleanup(client);
    mbedtls_platform_zeroize(auth,sizeof(auth)); return ok;
}

static unsigned voice_diag_error_result(task_voice_error_t error)
{
    if (error==TASK_VOICE_ERROR_AUTH) return VOICE_DIAG_RESULT_AUTH;
    if (error==TASK_VOICE_ERROR_TARGET_REQUIRED) return VOICE_DIAG_RESULT_ADMISSION;
    if (error==TASK_VOICE_ERROR_BUDGET) return VOICE_DIAG_RESULT_BUDGET;
    return VOICE_DIAG_RESULT_TRANSPORT;
}
static bool voice_cancel_requested(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_voice_cancel);
}
/* Preflight is N-only. S then covers capture/upload/cleanup; audio A is
   released before S is combined with N for the upload. TLS is cleaned before
   N is released, and scratch erase runs with S only. */
static void voice_record(unsigned diag_seq)
{
    unsigned diag_result=VOICE_DIAG_RESULT_INTERNAL,diag_capture=VOICE_DIAG_CAP_NONE;
    unsigned diag_flags=0,cancel_stage=VOICE_DIAG_RESULT_CANCEL_PREFLIGHT;
    if (voice_publish_unready()) {
        voice_diag_finalize(diag_seq,VOICE_DIAG_RESULT_UNREADY,diag_capture,diag_flags,0,-1); return;
    }
    if (s_creation.magic) {
        voice_publish(VOICE_PENDING,"新建请求已保存，待同步");
        voice_diag_finalize(diag_seq,VOICE_DIAG_RESULT_INTENT_PENDING,diag_capture,diag_flags,0,-1);
        return;
    }
    if (suite_connection_state()!=SUITE_CONN_ONLINE || !trusted()) {
        voice_publish(VOICE_FAILED,"请先联网校时，再语音新建");
        voice_diag_finalize(diag_seq,VOICE_DIAG_RESULT_OFFLINE,diag_capture,diag_flags,0,-1);
        return;
    }
    credentials_t config={0}; char *response=calloc(1,2049);
    suite_audio_lease_t *lease=NULL;
    bool net_held=false,audio_started=false; uint32_t bytes=0; int status=-1;
    s_voice_seconds=0; s_voice_title[0]=0; s_voice_name[0]=0;
    voice_publish(VOICE_CHECKING,"正在检查语音服务");
    if (!response) { diag_result=VOICE_DIAG_RESULT_ALLOC;
        voice_publish(VOICE_FAILED,voice_error_text(TASK_VOICE_ERROR_RETRY)); goto done; }
    if (!credentials(&config) || !config.write_token[0]) {
        diag_result=VOICE_DIAG_RESULT_AUTH;
        voice_publish(VOICE_FAILED,voice_error_text(TASK_VOICE_ERROR_AUTH)); goto done;
    }
    /* Preflight: N only. It must complete and release N before S is taken. */
    net_held=suite_net_take(1000);
    if (!net_held) { diag_result=VOICE_DIAG_RESULT_NETWORK; goto done; }
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_PREFLIGHT_START,false,false);
    bool preflight=voice_http(&config,"/suite/v1/task-voice",NULL,0,0,response,2049,
        &status,10000,diag_seq,1);
    voice_diag_status(diag_seq,status);
    suite_net_give(); net_held=false;
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_PREFLIGHT_DONE,true,true);
    if (!preflight || status!=200 ||
        !task_voice_response(response,strlen(response),false,s_voice_collection,s_voice_name,s_voice_title)) {
        task_voice_error_t error=task_voice_error_classify(status,response,strlen(response));
        diag_result=voice_diag_error_result(error);
        voice_publish(VOICE_FAILED,voice_error_text(error));
        goto done;
    }
    if (atomic_load(&s_voice_cancel)) {
        diag_result=VOICE_DIAG_RESULT_CANCEL_PREFLIGHT; goto done;
    }
    cancel_stage=VOICE_DIAG_RESULT_CANCEL_CAPTURE;
    if (suite_audio_job_acquire_ex(SUITE_AUDIO_OWNER_TASKS_VOICE,1000,
            voice_cancel_requested,NULL,&lease)!=ESP_OK) {
        diag_result=atomic_load(&s_voice_cancel) ? VOICE_DIAG_RESULT_CANCEL_PREFLIGHT :
            VOICE_DIAG_RESULT_STORAGE;
        voice_publish(VOICE_FAILED,voice_error_text(TASK_VOICE_ERROR_RETRY));
        goto done;
    }
    if (suite_audio_job_stage_begin(lease,TASK_VOICE_ERASE)!=ESP_OK) {
        s_voice_cache_blocked=true;
        diag_result=VOICE_DIAG_RESULT_STORAGE; goto done;
    }
    audio_started=true; diag_flags|=VOICE_DIAG_FLAG_AUDIO_STARTED;
    if (bsp_audio_init()!=ESP_OK || bsp_audio_set_format(16000,16,1)!=ESP_OK) {
        diag_result=VOICE_DIAG_RESULT_AUDIO; goto done;
    }
    voice_publish(VOICE_RECORDING,"正在录音，OK结束，长按取消");
    int64_t deadline=esp_timer_get_time()+12000000;
    unsigned char chunk[512];
    bool audio_failed=false,flash_failed=false;
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_CAPTURE_START,false,false);
    while (bytes<TASK_VOICE_PCM_MAX && esp_timer_get_time()<deadline &&
           !atomic_load(&s_voice_stop) && !atomic_load(&s_voice_cancel)) {
        size_t n=TASK_VOICE_PCM_MAX-bytes; if (n>sizeof(chunk)) n=sizeof(chunk);
        if (bsp_audio_read(chunk,n)!=ESP_OK) { audio_failed=true; break; }
        if (suite_audio_job_append(lease,chunk,n)!=ESP_OK) { flash_failed=true; break; }
        bytes+=(uint32_t)n;
        unsigned seconds=bytes/32000;
        if (seconds!=s_voice_seconds) { s_voice_seconds=seconds; voice_publish(VOICE_RECORDING,"正在录音，OK结束，长按取消"); }
    }
    bool recorded=!audio_failed && !flash_failed;
    if (recorded) {
        if (bytes>=TASK_VOICE_PCM_MAX) diag_capture=VOICE_DIAG_CAP_BYTES_FULL;
        else if (atomic_load(&s_voice_stop)) diag_capture=VOICE_DIAG_CAP_STOP;
        else if (atomic_load(&s_voice_cancel)) diag_capture=VOICE_DIAG_CAP_CANCEL;
        else if (esp_timer_get_time()>=deadline) diag_capture=VOICE_DIAG_CAP_DEADLINE;
        else diag_capture=VOICE_DIAG_CAP_SHORT;
    } else diag_capture=audio_failed ? VOICE_DIAG_CAP_AUDIO_FAIL :
        flash_failed ? VOICE_DIAG_CAP_FLASH_FAIL : VOICE_DIAG_CAP_AUDIO_FAIL;
    voice_diag_capture(diag_seq,bytes,diag_capture);
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_CAPTURE_END,true,true);
    /* Release A before the upload combines S with N. */
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_RELEASE_START,false,false);
    if (bsp_audio_release()!=ESP_OK) {
        recorded=false; diag_capture=VOICE_DIAG_CAP_RELEASE_FAIL;
        diag_flags|=VOICE_DIAG_FLAG_RELEASE_FAILED;
    }
    audio_started=false;
    voice_diag_capture(diag_seq,bytes,diag_capture);
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_RELEASE_DONE,true,true);
    if (!recorded || bytes<1600 || atomic_load(&s_voice_cancel)) {
        if (audio_failed) diag_result=VOICE_DIAG_RESULT_AUDIO;
        else if (flash_failed) diag_result=VOICE_DIAG_RESULT_STORAGE;
        else if (!recorded) diag_result=VOICE_DIAG_RESULT_AUDIO;
        else if (bytes<1600) diag_result=VOICE_DIAG_RESULT_INTERNAL;
        else diag_result=VOICE_DIAG_RESULT_CANCEL_CAPTURE;
        goto done;
    }
    if (suite_audio_job_finalize(lease)!=ESP_OK) {
        diag_result=VOICE_DIAG_RESULT_STORAGE; goto done;
    }
    voice_publish(VOICE_RECOGNIZING,"正在识别，请稍候");
    cancel_stage=VOICE_DIAG_RESULT_CANCEL_UPLOAD;
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_UPLOAD_START,false,false);
    net_held=suite_net_take(1000);
    bool answered=false;
    if (net_held) {
        answered=voice_http(&config,"/suite/v1/task-transcriptions",NULL,
            suite_audio_job_base(lease),bytes+44,response,2049,&status,45000,diag_seq,2);
        /* voice_http cleaned the TLS client before returning. */
        suite_net_give(); net_held=false;
    }
    voice_diag_status(diag_seq,status);
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_UPLOAD_DONE,true,true);
    if (!atomic_load(&s_voice_cancel)) {
        if (answered && status==200 &&
            task_voice_response(response,strlen(response),true,s_voice_collection,s_voice_name,s_voice_title)) {
            diag_result=VOICE_DIAG_RESULT_PREVIEW;
            cancel_stage=VOICE_DIAG_RESULT_CANCEL_CLEANUP;
            voice_publish(VOICE_PREVIEW,"核对标题，OK打开创建确认");
        } else {
            task_voice_error_t error=task_voice_error_classify(status,response,strlen(response));
            diag_result=voice_diag_error_result(error);
            voice_publish(VOICE_FAILED,voice_error_text(error));
        }
    } else diag_result=(answered && status==200) ?
        VOICE_DIAG_RESULT_CANCEL_CLEANUP : cancel_stage;
done:
    if (audio_started) {
        diag_flags|=VOICE_DIAG_FLAG_AUDIO_STARTED;
        voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_RELEASE_START,false,false);
        if (bsp_audio_release()!=ESP_OK) {
            diag_capture=VOICE_DIAG_CAP_RELEASE_FAIL;
            diag_flags|=VOICE_DIAG_FLAG_RELEASE_FAILED;
        }
        audio_started=false;
        voice_diag_capture(diag_seq,bytes,diag_capture);
        voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_RELEASE_DONE,true,true);
    }
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_CLEANUP_START,false,false);
    /* N is released before S-only scratch erase; TLS was already cleaned. */
    if (net_held) { suite_net_give(); net_held=false; }
    mbedtls_platform_zeroize(&config,sizeof(config));
    if (response) { mbedtls_platform_zeroize(response,2049); free(response); }
    bool cleaned=false;
    if (lease) cleaned=suite_audio_job_release(lease)==ESP_OK;
    else cleaned=true;
    if (!cleaned) {
        s_voice_cache_blocked=true;
        diag_flags|=VOICE_DIAG_FLAG_CLEANUP_FAILED|VOICE_DIAG_FLAG_CACHE_BLOCKED;
    }
    /* Worker cleanup includes the network owner, configuration and response
       buffers; the timestamp is taken only after those complete. */
    voice_diag_mark(diag_seq,VOICE_DIAG_STAGE_CLEANUP_DONE,true,true);
    if (!cleaned) {
        diag_result=VOICE_DIAG_RESULT_CLEANUP_FAILED;
        voice_publish(VOICE_STORAGE_FAILED,"录音清理失败，请重启");
    } else if (atomic_load(&s_voice_cancel)) {
        s_voice_title[0]=0; voice_publish(VOICE_READY,"已取消，OK重新录音");
        if (diag_result==VOICE_DIAG_RESULT_PREVIEW) diag_result=VOICE_DIAG_RESULT_CANCEL_CLEANUP;
        else if (diag_result!=VOICE_DIAG_RESULT_CANCEL_PREFLIGHT &&
                 diag_result!=VOICE_DIAG_RESULT_CANCEL_CAPTURE &&
                 diag_result!=VOICE_DIAG_RESULT_CANCEL_UPLOAD &&
                 diag_result!=VOICE_DIAG_RESULT_CANCEL_CLEANUP)
            diag_result=cancel_stage;
    } else if (s_voice_state!=VOICE_PREVIEW && s_voice_state!=VOICE_FAILED) {
        voice_publish(VOICE_FAILED,"录音或识别失败，OK重试");
        if (diag_result==VOICE_DIAG_RESULT_NONE) diag_result=VOICE_DIAG_RESULT_INTERNAL;
    }
    voice_diag_finalize(diag_seq,diag_result,diag_capture,diag_flags,bytes,status);
    ESP_LOGI(TAG,"Voice result=%u, pcm=%u, stack remaining=%u, free=%u, largest=%u",(unsigned)s_voice_state,(unsigned)bytes,
        (unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
}
static void voice_process(void)
{
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    unsigned command=atomic_exchange(&s_voice_command,0);
    xSemaphoreGive(s_mutex);
    if (!command) return;
    if (command==1) {
        unsigned diag_seq=voice_diag_worker_start();
        if (atomic_load(&s_voice_cancel)) {
            bool unready=voice_publish_unready();
            if (!unready) voice_publish(VOICE_READY,"已取消，OK重新录音");
            voice_diag_finalize(diag_seq,unready ? VOICE_DIAG_RESULT_UNREADY :
                VOICE_DIAG_RESULT_CANCEL_PREWORKER,VOICE_DIAG_CAP_NONE,0,0,-1);
        } else voice_record(diag_seq);
        return;
    }
    if (command==4) {
        if (!s_creation.magic) {
            s_voice_title[0]=0;
            if (!voice_publish_unready()) voice_publish(VOICE_READY,"OK开始录音，最长12秒");
        }
        return;
    }
    if (command==5) { s_voice_retry=true; s_voice_due=0; s_voice_attempts=0; return; }
    if (command==6 && s_creation.magic && (s_creation.state==TASK_OP_SUCCEEDED || s_creation.state==TASK_OP_CONFLICT ||
        s_creation.state==TASK_OP_READONLY || s_creation.state==TASK_OP_NOT_FOUND)) {
        nvs_handle_t h;
        if (nvs_open_from_partition("taskcfg","taskvoice",NVS_READWRITE,&h)==ESP_OK) {
            esp_err_t result=nvs_erase_key(h,"intent"); if (result==ESP_OK) result=nvs_commit(h); nvs_close(h);
            if (result==ESP_OK) { memset(&s_creation,0,sizeof(s_creation)); s_voice_title[0]=0; voice_publish(VOICE_READY,"OK开始录音，最长12秒"); }
        }
        return;
    }
    if (command!=3 || s_creation.magic || s_voice_state!=VOICE_PREVIEW || !s_voice_journal_ready) return;
    task_creation_t *candidate=calloc(1,sizeof(*candidate));
    if (!candidate) { voice_publish(VOICE_FAILED,"内存不足，未创建任务"); return; }
    unsigned char random[16]; esp_fill_random(random,sizeof(random));
    for (unsigned i=0;i<16;i++) snprintf(candidate->operation_id+2*i,3,"%02x",random[i]);
    strcpy(candidate->collection_id,s_voice_collection); strcpy(candidate->summary,s_voice_title);
    candidate->state=TASK_OP_LOCAL; task_creation_seal(candidate);
    if (voice_save(candidate)) {
        s_creation=*candidate; s_voice_due=0; s_voice_attempts=0;
        voice_publish(VOICE_PENDING,"新建请求已保存，待同步");
    } else voice_publish(VOICE_PREVIEW,"保存失败，尚未创建，请重试");
    mbedtls_platform_zeroize(candidate,sizeof(*candidate)); free(candidate);
}
static void voice_sync(bool retry)
{
    if (retry) { s_voice_retry=true; s_voice_attempts=0; s_voice_due=0; }
    if (!s_voice_journal_ready || !s_creation.magic || s_creation.state==TASK_OP_SUCCEEDED ||
        s_creation.state==TASK_OP_CONFLICT || s_creation.state==TASK_OP_READONLY || s_creation.state==TASK_OP_NOT_FOUND ||
        ((s_creation.state==TASK_OP_AUTH || s_creation.state==TASK_OP_PAUSED) && !s_voice_retry) ||
        esp_timer_get_time()<s_voice_due || suite_connection_state()!=SUITE_CONN_ONLINE || !trusted()) return;
    struct { char request[1400],response[2049]; } *buf=calloc(1,sizeof(*buf));
    credentials_t config={0}; task_op_state_t next=TASK_OP_LOCAL; bool received=false; int status=0;
    unsigned char digest[32]; char hex[65];
    if (buf && credentials(&config) && config.write_token[0] && task_creation_json(&s_creation,buf->request,sizeof(buf->request),NULL,false) &&
        mbedtls_sha256((unsigned char *)buf->request,strlen(buf->request),digest,0)==0) {
        for (unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
        if (task_creation_json(&s_creation,buf->request,sizeof(buf->request),hex,s_voice_retry) && suite_net_take(1000)) {
            bool body=voice_http(&config,"/suite/v1/task-creations",buf->request,0,0,
                buf->response,sizeof(buf->response),&status,10000,0,0);
            suite_net_give();
            if (body && (status==200 || status==202)) received=task_creation_receipt(buf->response,strlen(buf->response),&s_creation,hex,&next);
            else if (body && status==400 &&
                task_voice_error_classify(status,buf->response,strlen(buf->response))==TASK_VOICE_ERROR_TARGET_REQUIRED) {
                /* Only a strict HTTP 400 admission rejection before any durable
                   row is safe to present as an explicitly discardable result. */
                next=TASK_OP_READONLY; received=true;
            }
            else if (body) received=task_operation_error(buf->response,strlen(buf->response),&next);
            if (status==401 || status==403) { next=TASK_OP_AUTH; received=true; }
        }
    }
    s_voice_retry=false;
    mbedtls_platform_zeroize(&config,sizeof(config));
    if (buf) { mbedtls_platform_zeroize(buf,sizeof(*buf)); free(buf); }
    if (!received) {
        if (++s_voice_attempts>=5) next=TASK_OP_PAUSED;
        else { s_voice_due=esp_timer_get_time()+8000000; return; }
    } else s_voice_attempts=0;
    s_voice_due=esp_timer_get_time()+8000000;
    if (s_creation.state!=(unsigned)next) {
        task_creation_t previous=s_creation;
        s_creation.state=(uint8_t)next; task_creation_seal(&s_creation);
        if (!voice_save(&s_creation)) {
            s_creation=previous; s_voice_journal_ready=false;
            voice_publish(VOICE_STORAGE_FAILED,"同步结果保存异常，保留请求"); return;
        }
    }
    voice_publish(next==TASK_OP_SUCCEEDED ? VOICE_SUCCEEDED : VOICE_PENDING,
        next==TASK_OP_SUCCEEDED ? "任务已新建，OK继续" : next==TASK_OP_CONFLICT ? "新建冲突，OK查看处理" :
        next==TASK_OP_AUTH ? "新建认证待修复，OK重试" : next==TASK_OP_READONLY || next==TASK_OP_NOT_FOUND ? "目标不可写，OK查看处理" :
        next==TASK_OP_PAUSED ? "新建已暂停，OK重试" : "新建请求已保存，待同步");
    if (next==TASK_OP_SUCCEEDED) atomic_store(&s_auto_refresh,true);
}

bool suite_tasks_voice(unsigned command)
{
    if (!s_task || command<1 || command>6) return false;
    if (command==2) { voice_diag_request(false); return true; }
    if (command==4) voice_diag_request(true);
    if (xSemaphoreTake(s_mutex,0)!=pdTRUE) return false;
    bool accepted=atomic_load(&s_voice_command)==0;
    if (command==1 && s_view.voice_state!=VOICE_READY && s_view.voice_state!=VOICE_FAILED) accepted=false;
    if (accepted) {
        if (command==1) {
            atomic_store(&s_voice_cancel,false); atomic_store(&s_voice_stop,false);
            s_view.voice_state=VOICE_CHECKING; voice_diag_accept();
        }
        atomic_store(&s_voice_command,command);
    }
    xSemaphoreGive(s_mutex);
    if (!accepted) return false;
    xTaskNotifyGive(s_task); return true;
}
