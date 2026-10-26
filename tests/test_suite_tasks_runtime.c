/* Real firmware download owner with scripted HTTP/Flash boundaries. */
#define SUITE_AUDIO_JOB_TEST 1
#include "../main/suite_audio_job.c"
#include "../main/suite_tasks.c"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>

/* Each group starts from a fully reset harness. A failed assertion abandons
   only that group, so one red regression cannot hide the other scenarios. */
static jmp_buf group_jump;
static const char *group_name;
static char group_failure[256];
static void group_fail(const char *expression,int line)
{
    snprintf(group_failure,sizeof(group_failure),"%s:%d: %s",group_name,line,expression);
    longjmp(group_jump,1);
}
#undef assert
#define assert(expression) do { if (!(expression)) group_fail(#expression,__LINE__); } while(0)

static uint8_t memory[2*TASK_SLOT_SIZE];
static esp_partition_t partition={.size=sizeof(memory)};
static int64_t monotonic;
static int64_t virtual_clock_step;
static bool voice_release_fail,voice_cancel_on_preview,voice_reentry_on_failed;
static unsigned voice_reentry_hooks;
static size_t heap_internal_before=40000U,heap_internal_live=32000U,heap_internal_after=36000U;
static size_t heap_largest_before=20000U,heap_largest_live=16000U,heap_largest_after=18000U;
static size_t heap_dma_before=24000U,heap_dma_live=18000U,heap_dma_after=20000U;
static size_t heap_dma_largest_before=12000U,heap_dma_largest_live=9000U,heap_dma_largest_after=10000U;
static int64_t heap_sample_cost_us,audio_read_cost_us,audio_release_cost_us;
static int64_t http_open_cost_us,http_write_cost_us,http_fetch_cost_us,http_read_cost_us,http_cleanup_cost_us;
static int64_t net_give_cost_us,zeroize_cost_us,last_net_give_at,last_zeroize_at;
static unsigned test_critical_depth,boundary_preemptions;
static bool boundary_preempt_cancel,boundary_cancel_after_open;
static bool online=true,complete=true,net_available=true,init_fail,network_live,open_no_mem;
static bool net_owner_held;
static bool cancel_rx,overflow_rx;
static bool chunked_overflow,cancel_cleanup,expire_rx,read_fail;
static unsigned calls,cleans,takes,gives;
static int http_status=200;
static unsigned read_at;
static unsigned last_read_bytes;
static bool posting,nvs_fail,voice_script,audio_released,voice_cancel_capture,credentials_fail;
static bool voice_clear_fail,transcribe_after_deadline,voice_cancel_cleanup;
static uint32_t voice_clear_fail_base;
static unsigned audio_reads,voice_posts,voice_stop_request_at_read,voice_cancel_request_at_read;
static int voice_status_preflight=200,voice_status_transcribe=200,transcribe_first_byte_ms;
static task_creation_t nvs_creation;
static char scripted_voice_response[1800];
static char voice_preflight_body[512],voice_transcribe_body[512],voice_creation_body[1024];
static size_t payload_bytes,voice_creation_body_bytes;
esp_err_t bsp_audio_init(void)
{
    assert(suite_audio_job_held() && suite_audio_job_owner()==SUITE_AUDIO_OWNER_TASKS_VOICE);
    assert(!network_live && !net_owner_held); /* never A with N */
    audio_reads=0; audio_released=false; return ESP_OK;
}
esp_err_t bsp_audio_set_format(uint32_t hz,uint8_t bits,uint8_t ch) { assert(hz==16000 && bits==16 && ch==1); return ESP_OK; }
esp_err_t bsp_audio_read(void *out,size_t n) {
    assert(suite_audio_job_held() && !net_owner_held);
    memset(out,0,n); audio_reads++;
    if (audio_read_cost_us) monotonic+=audio_read_cost_us;
    if (voice_stop_request_at_read) {
        if (audio_reads==voice_stop_request_at_read) assert(suite_tasks_voice(2));
    } else if (audio_reads==10) atomic_store(&s_voice_stop,true);
    if (voice_cancel_request_at_read) {
        if (audio_reads==voice_cancel_request_at_read) assert(suite_tasks_voice(4));
    } else if (voice_cancel_capture && audio_reads==3) atomic_store(&s_voice_cancel,true);
    return ESP_OK;
}
esp_err_t bsp_audio_release(void) {
    assert(suite_audio_job_held() && !net_owner_held);
    audio_released=true;
    if (audio_release_cost_us) monotonic+=audio_release_cost_us;
    return voice_release_fail ? ESP_FAIL : ESP_OK;
}

static char posted[1400];
static unsigned posted_bytes,nvs_writes,nvs_erases;
static task_operation_t nvs_ops[TASK_OP_CAPACITY];
static uint32_t nvs_sequence_value;
static const char *write_token="completion_abcdefghijklmnopqrstuvwxyz_0123456789";
static suite_time_tracker_t tracker={.state=SUITE_TIME_SYNCED,.last_sync_epoch=1792368000};
struct esp_http_client { esp_http_client_config_t cfg; };
static struct esp_http_client client;
static const char *const empty_payload="{\"schema\":1,\"snapshot_revision\":\"0123456789abcdef0123456789abcdef\","
    "\"selection_revision\":1,\"downloaded_at\":1792368000,\"state\":\"EMPTY\",\"read_only\":true,"
    "\"total\":0,\"offset\":0,\"next_offset\":null,\"items\":[]}";
static const char *payload=empty_payload;
suite_conn_state_t suite_connection_state(void) { return online ? SUITE_CONN_ONLINE : SUITE_CONN_OFF; }
int64_t suite_connection_now(void) { return 1792368010; }
const suite_time_tracker_t *suite_connection_time(void) { return &tracker; }
/* External critical-depth model: a scheduling preemption may not occur while
   a target critical section is active. The hook below reads only the existing
   business cancel flag, never a diagnostic-internal field. */
void test_critical_enter(void) { test_critical_depth++; }
void test_critical_exit(void) { assert(test_critical_depth); test_critical_depth--; }
int64_t esp_timer_get_time(void)
{
    if (boundary_preempt_cancel && !test_critical_depth && atomic_load(&s_voice_cancel)) {
        boundary_preempt_cancel=false; boundary_preemptions++;
        voice_process();
    }
    if (virtual_clock_step) monotonic+=virtual_clock_step;
    return monotonic;
}
bool suite_net_take(unsigned ms)
{
    (void)ms; takes++;
    if (!net_available) return false;
    assert(!net_owner_held);
    net_owner_held=true;
    return true;
}
void suite_net_give(void) {
    assert(net_owner_held && !network_live);
    net_owner_held=false;
    if (net_give_cost_us) monotonic+=net_give_cost_us;
    last_net_give_at=monotonic; gives++;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) {
    (void)t; if (heap_sample_cost_us) monotonic+=heap_sample_cost_us; return 3500;
}
static size_t heap_value(size_t before,size_t live,size_t after)
{ return network_live ? live : audio_released ? after : before; }
size_t heap_caps_get_free_size(uint32_t caps)
{
    if (heap_sample_cost_us) monotonic+=heap_sample_cost_us;
    if ((caps&(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))
        return heap_value(heap_internal_before,heap_internal_live,heap_internal_after);
    if (caps&MALLOC_CAP_DMA)
        return heap_value(heap_dma_before,heap_dma_live,heap_dma_after);
    assert(0); return 0;
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    if (heap_sample_cost_us) monotonic+=heap_sample_cost_us;
    if ((caps&(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))
        return heap_value(heap_largest_before,heap_largest_live,heap_largest_after);
    if (caps&MALLOC_CAP_DMA)
        return heap_value(heap_dma_largest_before,heap_dma_largest_live,heap_dma_largest_after);
    assert(0); return 0;
}
#define TEST_LOG_MAX 96
#define TEST_LOG_LINE 640
static char test_logs[TEST_LOG_MAX][TEST_LOG_LINE];
static unsigned test_log_count,test_log_dropped;
static void logs_reset(void) { test_log_count=0; test_log_dropped=0; }
void test_log(const char *tag,const char *format,...)
{
    (void)tag;
    if (test_log_count>=TEST_LOG_MAX) { test_log_dropped++; return; }
    va_list args; va_start(args,format);
    vsnprintf(test_logs[test_log_count],TEST_LOG_LINE,format,args);
    va_end(args); test_log_count++;
}
static unsigned logs_prefix_count(const char *prefix)
{
    unsigned count=0; size_t n=strlen(prefix);
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],prefix,n)) count++;
    return count;
}
static const char *log_first(const char *prefix)
{
    size_t n=strlen(prefix);
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],prefix,n)) return test_logs[i];
    assert(0); return NULL;
}
static bool log_field(const char *line,const char *key,long long *out)
{
    char pattern[32]; snprintf(pattern,sizeof(pattern)," %s=",key);
    const char *p=strstr(line,pattern); if (!p) return false;
    p+=strlen(pattern); char *end=NULL; long long value=strtoll(p,&end,0);
    if (end==p) return false;
    *out=value; return true;
}
static const char *diag_final_line(void)
{
    const char *found=NULL; unsigned count=0;
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],"TVDIAGF ",8)) {
        long long seq=0; assert(log_field(test_logs[i],"seq",&seq) && seq>0);
        count++; found=test_logs[i];
    }
    assert(count==1); return found;
}
static unsigned diag_final_count(void)
{
    unsigned count=0;
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],"TVDIAGF ",8)) count++;
    return count;
}
static void check_diag_stage_order(void)
{
    long long previous=-1; unsigned lines=0;
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],"TVDIAG ",7)) {
        long long stage=0; assert(log_field(test_logs[i],"stage",&stage));
        assert(stage>previous); previous=stage; lines++;
    }
    assert(lines>0 && lines+logs_prefix_count("TVDIAGF ")<=16);
}
static void check_diag_no_private_text(void)
{
    for (unsigned i=0;i<test_log_count;i++) {
        const char *line=test_logs[i];
        assert(!strstr(line,"https://") && !strstr(line,"Bearer"));
        assert(!strstr(line,"task-voice") && !strstr(line,"task-transcriptions"));
        assert(!strstr(line,"suite/v1") && !strstr(line,write_token));
        assert(!strstr(line,"买牛奶"));
        if (s_voice_collection[0]) assert(!strstr(line,s_voice_collection));
        if (s_creation.operation_id[0]) assert(!strstr(line,s_creation.operation_id));
    }
}
void mbedtls_platform_zeroize(void *p,size_t n) {
    if (zeroize_cost_us) monotonic+=zeroize_cost_us;
    last_zeroize_at=monotonic; memset(p,0,n);
}
int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char out[32],int sha224)
{ assert(!sha224 && n<1400 && (!memcmp(p,"{\"base_etag\":",13) || !memcmp(p,"{\"collection_id\":",17))); assert(!strstr((const char *)p,"payload_hash")); memset(out,0xab,32); return 0; }
void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { memset(ctx,0,sizeof(*ctx)); }
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { memset(ctx,0,sizeof(*ctx)); }
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx,int is224)
{ (void)is224; memset(ctx,0,sizeof(*ctx)); return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *ctx,const unsigned char *in,size_t n)
{ (void)ctx; (void)in; (void)n; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,unsigned char out[32])
{ (void)ctx; memset(out,0xcd,32); return 0; }
void esp_fill_random(void *p,size_t n) { static unsigned char value=1; memset(p,value++,n); }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)2; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem,TickType_t ticks)
{
    (void)ticks;
    if (sem==s_sem) {
        assert(!net_owner_held); /* never acquire S while N is held */
        return suite_audio_job_held() ? pdFALSE : pdTRUE;
    }
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    if (voice_cancel_on_preview && s_view.voice_state==VOICE_PREVIEW) {
        voice_cancel_on_preview=false;
        assert(suite_tasks_voice(4));
    }
    if (voice_reentry_on_failed && s_view.voice_state==VOICE_FAILED) {
        voice_reentry_on_failed=false; voice_reentry_hooks++;
        assert(suite_tasks_voice(1));
    }
    return pdTRUE;
}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *name)
{ (void)type; (void)subtype; assert(!strcmp(name,"taskkeys")); return &partition; }
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *part,nvs_sec_cfg_t *cfg)
{ (void)part; memset(cfg,0,sizeof(*cfg)); return ESP_OK; }
esp_err_t nvs_flash_secure_init_partition(const char *name,nvs_sec_cfg_t *cfg)
{ (void)cfg; assert(!strcmp(name,"taskcfg")); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *part,const char *space,int mode,nvs_handle_t *h)
{ (void)mode; assert(!strcmp(part,"taskcfg")); assert(!strcmp(space,"tasks") || !strcmp(space,"taskops") || !strcmp(space,"taskvoice")); *h=!strcmp(space,"tasks")?1:!strcmp(space,"taskops")?2:3; return ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h==1 || h==2 || h==3); }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *bytes)
{
    assert(h==1); if (credentials_fail && !strcmp(key,"origin")) return ESP_ERR_NVS_NOT_FOUND;
    const char *value=!strcmp(key,"origin") ? "https://example.test" : !strcmp(key,"token") ?
        "abcdefghijklmnopqrstuvwxyz_0123456789" : write_token;
    assert(*bytes>strlen(value)); strcpy(out,value); *bytes=strlen(value)+1; return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *out)
{ if (h==1) { assert(!strcmp(key,"schema")); *out=1; return ESP_OK; } assert(h==2); *out=nvs_sequence_value; return nvs_sequence_value ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u32(nvs_handle_t h,const char *key,uint32_t value)
{ (void)key; assert(h==2); nvs_sequence_value=value; return ESP_OK; }
static unsigned nvs_slot(const char *key)
{ assert(key[0]=='o' && key[1]=='p' && key[2]>='0' && key[2]<'0'+TASK_OP_CAPACITY && !key[3]); return (unsigned)(key[2]-'0'); }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *bytes)
{ if (h==3) { assert(!strcmp(key,"intent") && *bytes==sizeof(nvs_creation)); if (!nvs_creation.magic) return ESP_ERR_NVS_NOT_FOUND; memcpy(out,&nvs_creation,*bytes); return ESP_OK; } assert(h==2 && *bytes==sizeof(task_operation_t)); unsigned slot=nvs_slot(key); if (!nvs_ops[slot].magic) return ESP_ERR_NVS_NOT_FOUND; memcpy(out,&nvs_ops[slot],*bytes); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t bytes)
{ if (h==3) { assert(bytes==sizeof(nvs_creation)); if (nvs_fail) return ESP_FAIL; memcpy(&nvs_creation,value,bytes); return ESP_OK; } assert(h==2 && bytes==sizeof(task_operation_t)); nvs_writes++; if (nvs_fail) return ESP_FAIL; memcpy(&nvs_ops[nvs_slot(key)],value,bytes); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key)
{ if (h==3) { assert(!strcmp(key,"intent")); memset(&nvs_creation,0,sizeof(nvs_creation)); return ESP_OK; } assert(h==2); nvs_erases++; memset(&nvs_ops[nvs_slot(key)],0,sizeof(task_operation_t)); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { assert(h==2 || h==3); return nvs_fail ? ESP_FAIL : ESP_OK; }
esp_err_t esp_partition_read(const esp_partition_t *p,uint32_t at,void *out,size_t n)
{ assert(p==&partition && at+n<=sizeof(memory)); memcpy(out,memory+at,n); return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p,uint32_t at,const void *data,size_t n)
{ assert(p==&partition && at+n<=sizeof(memory)); memcpy(memory+at,data,n); return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p,uint32_t at,size_t n)
{
    assert(p==&partition && at+n<=sizeof(memory));
    if (voice_clear_fail && at==voice_clear_fail_base && *(const uint32_t *)(memory+at)==0x31445541U)
        return ESP_FAIL;
    memset(memory+at,255,n); return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(net_owner_held);
    assert(cfg->disable_auto_redirect && cfg->crt_bundle_attach==esp_crt_bundle_attach);
    /* The exact value is a request policy, not a harness invariant: slow
       transcription tests derive reachability from whatever is configured. */
    assert(cfg->timeout_ms>0 && cfg->timeout_ms<=60000 && !strncmp(cfg->url,"https://",8));
    if (init_fail) return NULL;
    client.cfg=*cfg; posting=false; network_live=true; return &client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value)
{
    assert(c==&client);
    if (!strcmp(key,"Authorization")) {
        if (posting || voice_script) { char expected[144]; snprintf(expected,sizeof(expected),"Bearer %s",write_token); assert(!strcmp(value,expected)); }
        else assert(!strcmp(value,"Bearer abcdefghijklmnopqrstuvwxyz_0123456789"));
    }
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int n)
{
    (void)c; (void)n;
    if (strstr(client.cfg.url,"/suite/v1/tasks/snapshot") ||
        strstr(client.cfg.url,"task-transcriptions"))
        assert(suite_audio_job_held()); /* S -> N */
    if (strstr(client.cfg.url,"task-voice") || strstr(client.cfg.url,"task-creations"))
        assert(!suite_audio_job_held()); /* preflight/completion are N-only */
    if (voice_script && strstr(client.cfg.url,"task-transcriptions")) { assert(audio_released); voice_posts++; } calls++; read_at=0; last_read_bytes=0; posted_bytes=0; posted[0]=0;
    if (open_no_mem) return ESP_ERR_NO_MEM;
    if (http_open_cost_us) monotonic+=http_open_cost_us;
    if (boundary_cancel_after_open) {
        boundary_cancel_after_open=false; suite_tasks_cancel();
    }
    return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c,int method)
{ assert(c==&client && method==HTTP_METHOD_POST); posting=true; return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c,const char *data,int length)
{
    assert(c==&client && posting);
    if (http_write_cost_us) monotonic+=http_write_cost_us;
    if (voice_script && strstr(client.cfg.url,"task-transcriptions")) return length;
    unsigned n=(unsigned)length>7 ? 7 : (unsigned)length; assert(posted_bytes+n<sizeof(posted)); memcpy(posted+posted_bytes,data,n); posted_bytes+=n; posted[posted_bytes]=0; return (int)n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    if (http_fetch_cost_us) monotonic+=http_fetch_cost_us;
    if (voice_script) {
        if (strstr(client.cfg.url,"task-voice")) {
            if (voice_preflight_body[0]) snprintf(scripted_voice_response,sizeof(scripted_voice_response),"%s",voice_preflight_body);
            else snprintf(scripted_voice_response,sizeof(scripted_voice_response),"{\"schema\":1,\"ready\":true,\"collection_id\":\"%064d\",\"collection_name\":\"Tasks\"}",1);
        } else if (strstr(client.cfg.url,"task-transcriptions")) {
            /* A real socket would not deliver a first byte before the
               configured client timeout. The stub derives reachability from
               the timeout actually set, not from a hard-coded constant. */
            if (transcribe_first_byte_ms) {
                monotonic+=(int64_t)transcribe_first_byte_ms*1000;
                if (client.cfg.timeout_ms<=transcribe_first_byte_ms) return -1;
            }
            if (transcribe_after_deadline) monotonic+=46000000;
            if (voice_transcribe_body[0]) snprintf(scripted_voice_response,sizeof(scripted_voice_response),"%s",voice_transcribe_body);
            else snprintf(scripted_voice_response,sizeof(scripted_voice_response),"{\"schema\":1,\"summary\":\"买牛奶\",\"collection_id\":\"%064d\",\"collection_name\":\"Tasks\"}",1);
        } else if (voice_creation_body_bytes) {
            size_t n=voice_creation_body_bytes;
            if (n>sizeof(scripted_voice_response)) n=sizeof(scripted_voice_response);
            memcpy(scripted_voice_response,voice_creation_body,n);
        } else if (voice_creation_body[0]) snprintf(scripted_voice_response,sizeof(scripted_voice_response),"%s",voice_creation_body);
        else snprintf(scripted_voice_response,sizeof(scripted_voice_response),"{\"schema\":1,\"operation_id\":\"%s\",\"collection_id\":\"%s\",\"payload_hash\":\"abababababababababababababababababababababababababababababababab\",\"state\":\"SUCCEEDED\"}",s_creation.operation_id,s_creation.collection_id);
        payload=scripted_voice_response;
        payload_bytes=(voice_creation_body_bytes && strstr(client.cfg.url,"task-creations")) ?
            voice_creation_body_bytes : strlen(scripted_voice_response);
    } else payload_bytes=strlen(payload);
    return overflow_rx ? TASK_PAGE_MAX+1 : chunked_overflow ? 0 : (int64_t)payload_bytes;
}
int esp_http_client_read(esp_http_client_handle_t c,char *out,int length)
{
    (void)c;
    if (http_read_cost_us) monotonic+=http_read_cost_us;
    if (read_fail && read_at) return -1;
    if (cancel_rx) suite_tasks_cancel();
    if (expire_rx) monotonic+=90000000;
    if (chunked_overflow) {
        memset(out,'x',(size_t)length); last_read_bytes+=(unsigned)length; return length;
    }
    unsigned n=(unsigned)payload_bytes-read_at;
    if (n>(unsigned)length) n=(unsigned)length;
    memcpy(out,payload+read_at,n); read_at+=n; last_read_bytes+=n; return n;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int ms)
{ assert(c==&client && ms>0 && ms<=60000); c->cfg.timeout_ms=ms; return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    if (voice_script && strstr(c->cfg.url,"task-voice")) return voice_status_preflight;
    if (voice_script && strstr(c->cfg.url,"task-transcriptions")) return voice_status_transcribe;
    return http_status;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { (void)c; return complete; }
void esp_http_client_cleanup(esp_http_client_handle_t c)
{
    cleans++; network_live=false;
    if (http_cleanup_cost_us) monotonic+=http_cleanup_cost_us;
    if (cancel_cleanup) suite_tasks_cancel();
    if (voice_cancel_cleanup && strstr(c->cfg.url,"task-transcriptions")) {
        voice_cancel_cleanup=false; atomic_store(&s_voice_cancel,true);
    }
}

static void reset_harness(void)
{
    suite_audio_job_test_reset();
    virtual_clock_step=0;
    memset(memory,255,sizeof(memory));
    memset(&s_cache,0,sizeof(s_cache)); memset(&s_journal,0,sizeof(s_journal));
    memset(&s_view,0,sizeof(s_view)); memset(&s_command,0,sizeof(s_command));
    memset(&s_creation,0,sizeof(s_creation)); memset(&nvs_creation,0,sizeof(nvs_creation));
    memset(nvs_ops,0,sizeof(nvs_ops)); memset(scripted_voice_response,0,sizeof(scripted_voice_response));
    memset(voice_preflight_body,0,sizeof(voice_preflight_body));
    memset(voice_transcribe_body,0,sizeof(voice_transcribe_body));
    memset(voice_creation_body,0,sizeof(voice_creation_body));
    payload_bytes=0; voice_creation_body_bytes=0;
    memset(posted,0,sizeof(posted));
    memset(s_op_attempts,0,sizeof(s_op_attempts)); memset(s_op_due,0,sizeof(s_op_due));
    memset(s_op_retry,0,sizeof(s_op_retry));
    memset(s_orphan_slots,0,sizeof(s_orphan_slots));
    partition.size=sizeof(memory); s_partition=&partition;
    task_io_t reset_io={.ctx=&partition,.read=io_read,.write=io_write,.erase=io_erase};
    assert(suite_audio_job_init()==ESP_OK);
    assert(suite_audio_job_bind(&s_cache,reset_io)==ESP_OK);
    s_mutex=(SemaphoreHandle_t)1; s_task=(TaskHandle_t)1;
    s_index=0; s_note_page=0; s_detail=false; s_selection=0;
    s_auth_halted=false; s_completion_configured=true; s_voice_cache_blocked=false;
    s_notice[0]=0; s_halted_state=TASKS_AUTH_FAILED;
    s_orphan_count=0; s_orphan_generation=0; s_orphan_sequence=0; s_orphan_mask=0;
    atomic_store(&s_refresh,false); atomic_store(&s_auto_refresh,false);
    atomic_store(&s_cancel,false); atomic_store(&s_busy,false);
    atomic_store(&s_voice_command,0); atomic_store(&s_voice_stop,false); atomic_store(&s_voice_cancel,false);
    s_voice_state=VOICE_READY; s_voice_journal_ready=false; s_voice_retry=false;
    s_voice_title[0]=0; s_voice_collection[0]=0; s_voice_name[0]=0; s_voice_message[0]=0;
    s_voice_seconds=0; s_voice_attempts=0; s_voice_due=0;
    online=true; complete=true; net_available=true; init_fail=false; network_live=false; open_no_mem=false;
    net_owner_held=false;
    cancel_rx=false; overflow_rx=false; chunked_overflow=false; cancel_cleanup=false;
    expire_rx=false; read_fail=false; nvs_fail=false; voice_script=false;
    credentials_fail=false;
    audio_released=false; voice_cancel_capture=false; voice_clear_fail=false;
    voice_clear_fail_base=0; voice_stop_request_at_read=0; voice_cancel_request_at_read=0;
    voice_release_fail=false; voice_cancel_on_preview=false; voice_reentry_on_failed=false;
    voice_reentry_hooks=0;
    heap_sample_cost_us=0; audio_read_cost_us=0; audio_release_cost_us=0;
    http_open_cost_us=0; http_write_cost_us=0; http_fetch_cost_us=0;
    http_read_cost_us=0; http_cleanup_cost_us=0; net_give_cost_us=0; zeroize_cost_us=0;
    last_net_give_at=0; last_zeroize_at=0;
    boundary_preempt_cancel=false; boundary_cancel_after_open=false;
    boundary_preemptions=0; test_critical_depth=0;
    transcribe_after_deadline=false; voice_cancel_cleanup=false; transcribe_first_byte_ms=0;
    voice_status_preflight=200; voice_status_transcribe=200; http_status=200;
    calls=cleans=takes=gives=0; read_at=0; last_read_bytes=0; posting=false;
    audio_reads=0; voice_posts=0; nvs_writes=0; nvs_erases=0; nvs_sequence_value=0;
    posted_bytes=0; monotonic=0; payload=empty_payload;
    tracker.state=SUITE_TIME_SYNCED; tracker.last_sync_epoch=1792368000;
    logs_reset();
}

static void open_harness_cache(void)
{
    task_io_t io={.ctx=&partition,.read=io_read,.write=io_write,.erase=io_erase};
    (void)task_cache_open(&s_cache,io);
    assert(suite_audio_job_bind(&s_cache,io)==ESP_OK);
}
static uint32_t prepare_cache(void)
{
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    open_harness_cache(); payload=empty_payload; http_status=200;
    assert(download(&config)==TASKS_READY && s_cache.valid);
    return s_cache.active.generation;
}
static uint32_t prepare_voice_ready(void)
{
    uint32_t generation=prepare_cache();
    voice_script=true; http_status=200;
    voice_load();
    assert(s_voice_journal_ready && s_voice_state==VOICE_READY && s_view.voice_state==VOICE_READY);
    return generation;
}
static void prepare_pending_via_device(void)
{
    (void)prepare_voice_ready();
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && !s_creation.magic && voice_posts==1);
    assert(suite_tasks_voice(3)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_creation.state==TASK_OP_LOCAL);
}
static void prepare_creation_intent(void)
{
    task_creation_t intent={0};
    memset(intent.operation_id,'a',32); memset(intent.collection_id,'b',64);
    strcpy(intent.summary,"买牛奶"); intent.state=TASK_OP_LOCAL; task_creation_seal(&intent);
    open_harness_cache(); voice_script=true;
    assert(voice_save(&intent)); s_creation=intent; s_voice_journal_ready=true;
    s_voice_due=0; s_voice_attempts=0;
    memset(s_voice_collection,'b',64); s_voice_collection[64]=0; strcpy(s_voice_name,"Tasks");
    voice_publish(VOICE_PENDING,"新建请求已保存，待同步");
}
static void check_voice_cleanup(uint32_t generation,unsigned prior_takes,unsigned prior_gives)
{
    assert(calls==cleans && !network_live);
    assert(takes-prior_takes==gives-prior_gives && prior_gives<=gives);
    assert(s_cache.valid && s_cache.active.generation==generation);
    assert(*(const uint32_t *)memory!=0x31445541U);
    assert(*(const uint32_t *)(memory+TASK_SLOT_SIZE)!=0x31445541U);
}

/* R1: the real voice worker must tolerate a slow transcription first byte
   whose wait exceeds the old fixed request timeout, and must fail closed when
   the total deadline is exhausted. */
static void r1_slow_transcription_within_budget(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset();
    transcribe_first_byte_ms=15000;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && voice_posts==1 && audio_released);
    assert(!s_creation.magic && !nvs_creation.magic && s_voice_journal_ready);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==1); /* PREVIEW */
    assert(log_field(final,"d_worker",&value) && value>=0);
    assert(log_field(final,"d_preflight",&value) && value>=0);
    assert(log_field(final,"d_capture",&value) && value>=0);
    assert(log_field(final,"d_capture_end",&value) && value>=0);
    assert(log_field(final,"d_release",&value) && value>=0);
    assert(log_field(final,"d_release_done",&value) && value>=0);
    assert(log_field(final,"d_upload",&value) && value>=0);
    assert(log_field(final,"d_cleanup",&value) && value>=0);
    assert(log_field(final,"d_cleanup_done",&value) && value>=0);
    assert(log_field(final,"d_total",&value) && value>=0);
    assert(log_field(final,"d_stop",&value) && value==-1);
    assert(log_field(final,"d_cancel",&value) && value==-1);
    assert(log_field(final,"rs",&value) && value==1);
    assert(log_field(final,"stack_b",&value) && value>0);
    check_diag_stage_order(); check_diag_no_private_text();
}
static void r1_deadline_exceeded_fails_closed(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset();
    transcribe_after_deadline=true;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_FAILED && !s_creation.magic && !nvs_creation.magic);
    assert(audio_released && s_voice_journal_ready);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==16); /* TRANSPORT */
    assert(log_field(final,"d_upload",&value) && value>=0);
    assert(log_field(final,"d_cleanup",&value) && value>=0);
    check_diag_stage_order();
}

/* A complete transcription response whose cleanup races a user cancel must
   never become a preview or a creation intent. */
static void r1_cancel_after_complete_response(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset();
    voice_cancel_cleanup=true;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_READY && strstr(s_voice_message,"取消"));
    assert(!s_creation.magic && !nvs_creation.magic && s_voice_journal_ready);
    assert(audio_released);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==6); /* CANCEL_CLEANUP */
    check_diag_stage_order();
}

/* R2: an explicit pre-admission rejection is discardable, while an unknown
   error, a full queue or a lost response keeps the durable intent. */
static void r2_creation_target_required_is_discardable(void)
{
    reset_harness(); prepare_pending_via_device();
    http_status=400; snprintf(voice_creation_body,sizeof(voice_creation_body),"%s","{\"error\":\"CREATION_TARGET_REQUIRED\"}");
    voice_creation_body_bytes=strlen(voice_creation_body);
    voice_sync(false);
    assert(s_creation.magic && nvs_creation.magic);
    assert(suite_tasks_voice(6)); voice_process();
    assert(!s_creation.magic && !nvs_creation.magic && s_voice_journal_ready);
    assert(s_voice_state==VOICE_READY);
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && !s_creation.magic);
}
static void r2_unknown_400_retains_intent(void)
{
    reset_harness(); prepare_pending_via_device();
    http_status=400; snprintf(voice_creation_body,sizeof(voice_creation_body),"%s","{\"error\":\"SOMETHING_NEW\"}");
    voice_sync(false);
    assert(s_creation.state==TASK_OP_PAUSED && nvs_creation.state==TASK_OP_PAUSED);
    assert(suite_tasks_voice(6)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_voice_journal_ready);
}
static void r2_queue_full_retains_intent(void)
{
    reset_harness(); prepare_pending_via_device();
    http_status=400; snprintf(voice_creation_body,sizeof(voice_creation_body),"%s","{\"error\":\"QUEUE_FULL\"}");
    voice_sync(false);
    assert(s_creation.state==TASK_OP_PAUSED && nvs_creation.state==TASK_OP_PAUSED);
    assert(suite_tasks_voice(6)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_voice_journal_ready);
}
static void r2_lost_response_retains_intent(void)
{
    reset_harness(); prepare_pending_via_device();
    http_status=200; complete=false;
    voice_sync(false);
    assert(s_creation.state==TASK_OP_LOCAL && nvs_creation.state==TASK_OP_LOCAL);
    assert(suite_tasks_voice(6)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_voice_journal_ready);
}

static void voice_creation_rejection_not_discardable(int status,const char *body)
{
    reset_harness(); prepare_pending_via_device();
    http_status=status; snprintf(voice_creation_body,sizeof(voice_creation_body),"%s",body);
    voice_creation_body_bytes=strlen(voice_creation_body);
    voice_sync(false);
    assert(s_creation.magic && nvs_creation.magic);
    assert(s_creation.state!=TASK_OP_READONLY && s_creation.state!=TASK_OP_CONFLICT &&
           s_creation.state!=TASK_OP_NOT_FOUND);
    assert(suite_tasks_voice(6)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_voice_journal_ready);
}
static void r2_duplicate_error_key_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"error\":\"CREATION_TARGET_REQUIRED\",\"error\":\"CREATION_TARGET_REQUIRED\"}"); }
static void r2_trailing_garbage_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"error\":\"CREATION_TARGET_REQUIRED\"}x"); }
static void r2_missing_error_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"detail\":\"CREATION_TARGET_REQUIRED\"}"); }
static void r2_non_string_error_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"error\":7}"); }
static void r2_503_same_string_retains_intent(void)
{ voice_creation_rejection_not_discardable(503,"{\"error\":\"CREATION_TARGET_REQUIRED\"}"); }
static void r2_escaped_nul_value_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"error\":\"CREATION_TARGET_REQUIRED\\u0000extra\"}"); }
static void r2_escaped_nul_key_retains_intent(void)
{ voice_creation_rejection_not_discardable(400,"{\"error\\u0000extra\":\"CREATION_TARGET_REQUIRED\"}"); }
static void r2_raw_nul_body_retains_intent(void)
{
    static const char body[]="{\"error\":\"CREATION_TARGET_REQUIRED\"}\0tail";
    reset_harness(); prepare_pending_via_device();
    http_status=400;
    memcpy(voice_creation_body,body,sizeof(body)-1);
    voice_creation_body_bytes=sizeof(body)-1;
    voice_sync(false);
    assert(s_creation.magic && nvs_creation.magic);
    assert(s_creation.state!=TASK_OP_READONLY && s_creation.state!=TASK_OP_CONFLICT &&
           s_creation.state!=TASK_OP_NOT_FOUND);
    assert(suite_tasks_voice(6)); voice_process();
    assert(s_creation.magic && nvs_creation.magic && s_voice_journal_ready);
}

/* R3: non-200 bodies must at least separate authentication repair, quota and
   transient failure without ever reaching preview or creating an intent. */
static void voice_non_200_case(int preflight_status,int transcribe_status,
                               const char *body,const char *kind)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    unsigned prior_reads=audio_reads,prior_posts=voice_posts;
    unsigned prior_writes=nvs_writes,prior_erases=nvs_erases;
    if (preflight_status!=200) {
        voice_status_preflight=preflight_status;
        snprintf(voice_preflight_body,sizeof(voice_preflight_body),"%s",body);
    } else {
        voice_status_transcribe=transcribe_status;
        snprintf(voice_transcribe_body,sizeof(voice_transcribe_body),"%s",body);
    }
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state!=VOICE_PREVIEW && !s_creation.magic && !nvs_creation.magic);
    assert(s_view.voice_state!=VOICE_CHECKING && s_view.voice_state!=VOICE_RECORDING &&
           s_view.voice_state!=VOICE_RECOGNIZING);
    assert(s_voice_journal_ready && s_voice_message[0]);
    if (!strcmp(kind,"auth")) assert(strstr(s_voice_message,"认证"));
    else if (!strcmp(kind,"budget")) assert(strstr(s_voice_message,"额度"));
    else if (!strcmp(kind,"transient")) assert(strstr(s_voice_message,"重试"));
    else if (!strcmp(kind,"target")) {
        assert(s_voice_state==VOICE_FAILED);
        assert(!strcmp(s_voice_message,"语音未配置，或目标列表不可写"));
        assert(!strcmp(s_view.voice_message,"语音未配置，或目标列表不可写"));
    }
    else assert(!strstr(s_voice_message,"认证") && !strstr(s_voice_message,"额度"));
    if (preflight_status!=200) {
        assert(audio_reads==prior_reads && voice_posts==prior_posts && !audio_released);
        assert(nvs_writes==prior_writes && nvs_erases==prior_erases);
    } else
        assert(audio_reads>prior_reads && voice_posts==prior_posts+1 && audio_released);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    if (!strcmp(kind,"target")) {
        const char *final=diag_final_line(); long long result=0;
        assert(log_field(final,"result",&result) && result==11); /* ADMISSION */
    }
}
static void r3_preflight_write_token_401(void)
{ voice_non_200_case(401,200,"{\"error\":\"UNAUTHORIZED\"}","auth"); }
static void r3_preflight_creation_target_required(void)
{ voice_non_200_case(400,200,"{\"error\":\"CREATION_TARGET_REQUIRED\"}","target"); }
static void r3_transcription_auth_failed(void)
{ voice_non_200_case(200,400,"{\"error\":\"SPEECH_AUTH_FAILED\"}","auth"); }
static void r3_transcription_budget(void)
{ voice_non_200_case(200,400,"{\"error\":\"SPEECH_BUDGET_EXCEEDED\"}","budget"); }
static void r3_transcription_unavailable(void)
{ voice_non_200_case(200,400,"{\"error\":\"SPEECH_UNAVAILABLE\"}","transient"); }
static void r3_transcription_timeout(void)
{ voice_non_200_case(200,400,"{\"error\":\"SPEECH_TIMEOUT\"}","transient"); }
static void r3_transcription_unknown(void)
{ voice_non_200_case(200,400,"{\"error\":\"SOMETHING_NEW\"}","unknown"); }
static void r3_transcription_malformed(void)
{ voice_non_200_case(200,400,"{\"error\":","unknown"); }

/* R4: a journal that is not ready must stay visibly failed, and a durable
   receipt that could not be saved must replay the same ID after reopen. */
static void r4_journal_not_ready_command1(void)
{
    reset_harness(); (void)prepare_cache();
    s_completion_configured=false; voice_script=true; voice_load();
    assert(!s_voice_journal_ready && s_voice_state==VOICE_FAILED);
    unsigned prior_takes=takes,prior_posts=voice_posts,prior_reads=audio_reads;
    logs_reset();
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_view.voice_state==VOICE_FAILED || s_view.voice_state==VOICE_STORAGE_FAILED);
    assert(s_view.voice_state!=VOICE_CHECKING);
    assert(audio_reads==prior_reads && voice_posts==prior_posts && takes==prior_takes);
    assert(!s_creation.magic && !nvs_creation.magic);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==7); /* UNREADY */
    assert(log_field(final,"d_worker",&value) && value>=0);
    assert(log_field(final,"d_preflight",&value) && value==-1);
    assert(logs_prefix_count("TVDIAG ")==1); /* START only */
}
static void r4_cancel_before_worker_keeps_unready_failure(void)
{
    reset_harness(); (void)prepare_cache();
    s_completion_configured=false; voice_script=true; voice_load();
    assert(!s_voice_journal_ready && s_voice_state==VOICE_FAILED);
    unsigned prior_takes=takes,prior_posts=voice_posts,prior_reads=audio_reads;
    assert(suite_tasks_voice(1));
    assert(!suite_tasks_voice(4)); /* command 1 still occupies the slot */
    voice_process();
    assert(!s_voice_journal_ready);
    assert(s_view.voice_state==VOICE_FAILED || s_view.voice_state==VOICE_STORAGE_FAILED);
    assert(s_view.voice_state!=VOICE_READY && s_view.voice_state!=VOICE_CHECKING);
    assert(audio_reads==prior_reads && voice_posts==prior_posts && takes==prior_takes);
    assert(nvs_writes==0 && nvs_erases==0 && !nvs_creation.magic);
}
static void r4_command4_does_not_mask_missing_journal(void)
{
    reset_harness(); (void)prepare_cache();
    s_completion_configured=false; voice_script=true; voice_load();
    assert(!s_voice_journal_ready && s_voice_state==VOICE_FAILED);
    assert(suite_tasks_voice(4)); voice_process();
    assert(!s_voice_journal_ready);
    assert(s_view.voice_state!=VOICE_READY);
    if (suite_tasks_voice(1)) {
        voice_process();
        assert(s_view.voice_state==VOICE_FAILED || s_view.voice_state==VOICE_STORAGE_FAILED);
    }
}
static void r4_cache_blocked_stays_terminal(void)
{
    reset_harness(); open_harness_cache();
    *(uint32_t *)memory=0x31445541U; voice_clear_fail=true; voice_script=true;
    voice_load();
    assert(s_voice_cache_blocked && !s_voice_journal_ready && s_voice_state==VOICE_STORAGE_FAILED);
    voice_clear_fail=false;
    unsigned prior_takes=takes,prior_posts=voice_posts,prior_reads=audio_reads;
    assert(!suite_tasks_voice(1)); voice_process();
    assert(s_view.voice_state==VOICE_STORAGE_FAILED && s_view.voice_state!=VOICE_CHECKING);
    assert(audio_reads==prior_reads && voice_posts==prior_posts && takes==prior_takes);
}
static void r4_succeeded_save_failure_replays_same_id(void)
{
    reset_harness(); prepare_creation_intent();
    unsigned prior_posts=voice_posts,prior_takes=takes,prior_gives=gives;
    char operation[33]; strcpy(operation,s_creation.operation_id);
    http_status=200; complete=true; nvs_fail=true;
    voice_sync(false);
    assert(s_creation.magic && s_creation.state==TASK_OP_LOCAL);
    assert(!s_voice_journal_ready && s_view.voice_state==VOICE_STORAGE_FAILED);
    assert(nvs_creation.magic && nvs_creation.state==TASK_OP_LOCAL);
    nvs_fail=false; voice_load();
    /* A normal reboot clears volatile retry scheduling. */
    s_voice_due=0; s_voice_attempts=0;
    assert(s_voice_journal_ready && s_voice_state==VOICE_PENDING);
    assert(!strcmp(operation,s_creation.operation_id));
    voice_sync(false);
    assert(s_creation.state==TASK_OP_SUCCEEDED && nvs_creation.state==TASK_OP_SUCCEEDED);
    assert(!strcmp(operation,s_creation.operation_id) && !strcmp(operation,nvs_creation.operation_id));
    assert(voice_posts==prior_posts && calls==cleans && !network_live);
    assert(takes-prior_takes==gives-prior_gives);
}

/* Diagnostics: one accepted attempt emits ordered numeric markers, one final
   result and a bounded line count. A pre-worker cancel must not leak markers
   into the next attempt or turn the microphone on. */
static void diag_preworker_cancel_no_stale(void)
{
    reset_harness();
    (void)prepare_voice_ready();
    unsigned reads=audio_reads,posts=voice_posts,http_calls=calls;
    logs_reset();
    assert(suite_tasks_voice(1));
    assert(!suite_tasks_voice(4)); /* command 1 still occupies the slot */
    voice_process();
    assert(s_voice_state==VOICE_READY && strstr(s_voice_message,"取消"));
    assert(audio_reads==reads && voice_posts==posts && calls==http_calls);
    const char *first=diag_final_line(); long long seq1=0,value=0;
    assert(log_field(first,"seq",&seq1) && seq1>0);
    assert(log_field(first,"result",&value) && value==2); /* CANCEL_PREWORKER */
    assert(log_field(first,"d_cancel",&value) && value>=0);
    assert(log_field(first,"d_stop",&value) && value==-1);
    assert(log_field(first,"d_capture",&value) && value==-1);
    check_diag_stage_order(); check_diag_no_private_text();

    logs_reset();
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW);
    const char *second=diag_final_line(); long long seq2=0;
    assert(log_field(second,"seq",&seq2) && seq2>seq1);
    assert(log_field(second,"d_cancel",&value) && value==-1);
    assert(log_field(second,"d_stop",&value) && value==-1);
    check_diag_stage_order(); check_diag_no_private_text();
}
/* The stop request and capture end are separate observations: the request is
   recorded at the input boundary, capture end only after the worker exits. */
static void diag_capture_stop_separates_request(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset(); voice_stop_request_at_read=4;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && audio_released);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==1); /* PREVIEW */
    assert(log_field(final,"cap",&value) && value==2); /* STOP */
    assert(log_field(final,"d_stop",&value) && value>=0);
    assert(log_field(final,"d_stop_edge",&value) && value>=0);
    assert(log_field(final,"d_capture",&value) && value>=0);
    assert(log_field(final,"d_release",&value) && value>=0);
    assert(log_field(final,"d_upload",&value) && value>=0);
    check_diag_stage_order();
}
static void diag_capture_cancel_release_cleanup(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset(); voice_cancel_request_at_read=3;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_READY && audio_released && voice_posts==0);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==4); /* CANCEL_CAPTURE */
    assert(log_field(final,"cap",&value) && value==3); /* CANCEL */
    assert(log_field(final,"d_cancel",&value) && value>=0);
    assert(log_field(final,"d_cancel_edge",&value) && value>=0);
    assert(log_field(final,"d_release",&value) && value>=0);
    assert(log_field(final,"d_cleanup",&value) && value>=0);
    assert(log_field(final,"d_upload",&value) && value==-1);
    check_diag_stage_order();
}
static void diag_cleanup_failure_is_terminal(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    voice_clear_fail=true;
    voice_clear_fail_base=(uint32_t)(1-s_cache.slot)*TASK_SLOT_SIZE;
    logs_reset();
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_STORAGE_FAILED && s_voice_cache_blocked);
    assert(calls==cleans && !network_live);
    assert(takes-prior_takes==gives-prior_gives && prior_gives<=gives);
    assert(s_cache.valid && s_cache.active.generation==generation);
    assert(*(const uint32_t *)(memory+voice_clear_fail_base)==0x31445541U); /* quarantine retained */
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==17); /* CLEANUP_FAILED */
    assert(log_field(final,"flags",&value) && (value&4));
    assert(log_field(final,"d_cleanup",&value) && value>=0);
    check_diag_stage_order();
}
/* Early download returns must name the cause without pretending an HTTP
   attempt happened; a real download may sample TLS while the client is alive. */
static void diag_download_early_no_http(void)
{
    reset_harness();
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    s_partition=&partition;
    long long value=0;
    logs_reset(); online=false;
    assert(download(&config)==TASKS_OFFLINE);
    assert(logs_prefix_count("DLDIAG ")==1 && logs_prefix_count("DLTLS ")==0);
    const char *line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==3); /* OFFLINE */
    assert(log_field(line,"http",&value) && value==0);
    assert(log_field(line,"req",&value) && value==0);
    assert(log_field(line,"pages",&value) && value==0);

    logs_reset(); online=true; tracker.state=SUITE_TIME_UNSYNCED;
    assert(download(&config)==TASKS_TIME_REQUIRED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==4);
    assert(log_field(line,"http",&value) && value==0);
    tracker.state=SUITE_TIME_SYNCED; net_available=false;
    logs_reset(); assert(download(&config)==TASKS_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==6); /* NET */
    assert(log_field(line,"req",&value) && value==0);
    net_available=true; s_auth_halted=true; s_halted_state=TASKS_AUTH_FAILED;
    logs_reset(); assert(download(&config)==TASKS_AUTH_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==5);
    s_auth_halted=false; s_voice_cache_blocked=true;
    logs_reset(); assert(download(&config)==TASKS_STORAGE_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==2);
    s_voice_cache_blocked=false;

    open_harness_cache(); payload=empty_payload; http_status=200; complete=true;
    logs_reset(); assert(download(&config)==TASKS_READY);
    assert(logs_prefix_count("DLDIAG ")==1);
    assert(logs_prefix_count("DLTLS ")>=1 && logs_prefix_count("DLTLS ")<=4);
    assert(logs_prefix_count("TVDIAG ")==0 && logs_prefix_count("TVDIAGF ")==0);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==1); /* READY */
    assert(log_field(line,"http",&value) && value>=1);
    assert(log_field(line,"req",&value) && value>=1);
    assert(log_field(line,"pages",&value) && value>=1);
    assert(log_field(line,"stack_b",&value) && value>0);
}

/* Codex review race: a terminal publish may wake a new accepted start before
   the old worker finalizes. Both attempts must retain exactly one final event. */
static void diag_terminal_reentry_keeps_both_finals(void)
{
    reset_harness();
    (void)prepare_voice_ready();
    logs_reset();
    online=false;
    voice_reentry_on_failed=true;
    assert(suite_tasks_voice(1));
    voice_process();
    assert(voice_reentry_hooks==1 && !voice_reentry_on_failed);
    assert(diag_final_count()==1);
    voice_process(); /* process the accepted reentry */
    assert(diag_final_count()==2);
    long long seq[2]={0}; unsigned finals=0;
    for (unsigned i=0;i<test_log_count;i++) if (!strncmp(test_logs[i],"TVDIAGF ",8)) {
        assert(finals<2);
        assert(log_field(test_logs[i],"seq",&seq[finals]) && seq[finals]>0);
        long long value=0;
        assert(log_field(test_logs[i],"result",&value) && value==9); /* OFFLINE */
        assert(log_field(test_logs[i],"flags",&value) && (value&0x80000000LL));
        finals++;
    }
    assert(finals==2 && seq[0]!=seq[1]);
    check_diag_no_private_text();
}

/* A cancel accepted at PREVIEW is still a real request: record it even while
   the worker is finishing the response/cleanup. */
static void diag_preview_cancel_request_recorded(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset();
    voice_cancel_on_preview=true;
    assert(suite_tasks_voice(1)); voice_process();
    assert(!voice_cancel_on_preview);
    assert(s_voice_state==VOICE_READY && strstr(s_voice_message,"取消"));
    assert(voice_posts==1);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==6); /* CANCEL_CLEANUP */
    assert(log_field(final,"d_cancel",&value) && value>=0);
    assert(log_field(final,"valid",&value) && (value&(1LL<<13)));
    assert(log_field(final,"flags",&value) && (value&2));
    voice_process(); /* drain the queued cancel command */
}

/* Nonzero fake time makes the boundary order observable: boundary timestamps
   precede resource sampling, upload phases are separate, and cleanup ends
   after net/config/response release. */
static void diag_timing_boundaries_and_upload(void)
{
    reset_harness();
    uint32_t generation=prepare_voice_ready();
    unsigned prior_takes=takes,prior_gives=gives;
    logs_reset();
    heap_sample_cost_us=100; audio_read_cost_us=1000; audio_release_cost_us=500;
    http_open_cost_us=2000; http_write_cost_us=300; http_fetch_cost_us=7000;
    http_read_cost_us=100; http_cleanup_cost_us=400;
    net_give_cost_us=300; zeroize_cost_us=100;
    voice_stop_request_at_read=4;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && audio_released && voice_posts==1);
    check_voice_cleanup(generation,prior_takes,prior_gives);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"d_capture",&value) && value==4000);
    assert(log_field(final,"d_capture_end",&value) && value>0);
    assert(log_field(final,"d_stop",&value) && value>=0);
    assert(log_field(final,"d_stop_edge",&value) && value==0);
    assert(log_field(final,"d_release",&value) && value==500);
    assert(log_field(final,"d_release_done",&value) && value>0);
    /* Network is released before S-only scratch cleanup, so this interval now
       contains the two bounded zeroizations rather than a net wait. */
    assert(log_field(final,"d_cleanup",&value) && value==200);
    assert(log_field(final,"d_cleanup_done",&value) && value>=200);
    assert(log_field(final,"d_up_open",&value) && value==2000);
    assert(log_field(final,"d_up_send",&value) && value==1500);
    assert(log_field(final,"d_up_wait",&value) && value==7000);
    assert(log_field(final,"d_up_tail",&value) && value>=600);
    assert(log_field(final,"t",&value) && value>=last_net_give_at && value>=last_zeroize_at);

    unsigned samples=0; bool before=false,live=false,after=false;
    for (unsigned i=0;i<test_log_count;i++) {
        if (strncmp(test_logs[i],"TVDIAG",6) && strncmp(test_logs[i],"TVDIAGF",7)) continue;
        long long rs=0,ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
        if (!log_field(test_logs[i],"rs",&rs) || rs!=1) continue;
        assert(log_field(test_logs[i],"ifree_b",&ifree) && ifree>0);
        assert(log_field(test_logs[i],"ilarge_b",&ilarge) && ilarge>0);
        assert(log_field(test_logs[i],"dfree_b",&dfree) && dfree>0);
        assert(log_field(test_logs[i],"dlarge_b",&dlarge) && dlarge>0);
        assert(log_field(test_logs[i],"stack_b",&stack) && stack==3500);
        assert(ifree!=dfree && ilarge!=dlarge);
        if (ifree==32000) live=true; else if (ifree==40000) before=true; else if (ifree==36000) after=true;
        else assert(0);
        samples++;
    }
    assert(samples==8 && before && live && after);
    check_diag_no_private_text();
}

/* Release failure must remain explicit rather than being reported as a clean
   hardware stop. */
static void diag_release_failure_is_explicit(void)
{
    reset_harness();
    (void)prepare_voice_ready();
    logs_reset();
    voice_release_fail=true;
    assert(suite_tasks_voice(1)); voice_process();
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==14); /* AUDIO */
    assert(log_field(final,"cap",&value) && value==7); /* RELEASE_FAIL */
    assert(log_field(final,"flags",&value) && (value&0x20));
    assert(log_field(final,"d_release",&value) && value>=0);
    check_diag_no_private_text();
}

/* A missing/unconfigured credential store must be visible numerically before
   any HTTP/client or microphone stage. */
static void diag_credentials_unconfigured_numeric(void)
{
    reset_harness();
    (void)prepare_voice_ready();
    logs_reset();
    credentials_fail=true;
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_FAILED && strstr(s_voice_message,"认证"));
    assert(audio_reads==0 && voice_posts==0 && !network_live);
    const char *final=diag_final_line(); long long value=0;
    assert(log_field(final,"result",&value) && value==10); /* AUTH */
    assert(log_field(final,"status",&value) && value==-1);
    assert(log_field(final,"d_preflight",&value) && value==-1);
    assert(log_field(final,"d_capture",&value) && value==-1);
    credentials_fail=false;
    check_diag_no_private_text();
}

/* Download completion is a separate bit: stage stays at the last reached
   boundary, and open/response counts distinguish attempts from real I/O. */
static void diag_download_stage_and_boundaries(void)
{
    reset_harness();
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    s_partition=&partition;
    long long value=0;
    logs_reset(); online=false;
    assert(download(&config)==TASKS_OFFLINE);
    const char *line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==3);
    assert(log_field(line,"done",&value) && value==0);
    assert(log_field(line,"stage",&value) && value==1);
    assert(log_field(line,"http",&value) && value==0);
    assert(log_field(line,"open",&value) && value==0);
    assert(log_field(line,"resp",&value) && value==0);

    online=true; tracker.state=SUITE_TIME_UNSYNCED; logs_reset();
    assert(download(&config)==TASKS_TIME_REQUIRED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==4);
    assert(log_field(line,"done",&value) && value==0);
    tracker.state=SUITE_TIME_SYNCED; net_available=false; logs_reset();
    assert(download(&config)==TASKS_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==6);
    assert(log_field(line,"open",&value) && value==0);
    net_available=true; s_auth_halted=true; s_halted_state=TASKS_AUTH_FAILED; logs_reset();
    assert(download(&config)==TASKS_AUTH_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==5);
    assert(log_field(line,"done",&value) && value==0);
    s_auth_halted=false; s_voice_cache_blocked=true; logs_reset();
    assert(download(&config)==TASKS_STORAGE_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==2);
    assert(log_field(line,"done",&value) && value==0);
    s_voice_cache_blocked=false;

    open_harness_cache(); payload=empty_payload; http_status=200; complete=true;
    logs_reset(); assert(download(&config)==TASKS_READY);
    line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==1);
    assert(log_field(line,"done",&value) && value==1);
    assert(log_field(line,"stage",&value) && value==8); /* COMMIT */
    assert(log_field(line,"http",&value) && value>=1);
    assert(log_field(line,"open",&value) && value>=1);
    assert(log_field(line,"resp",&value) && value>=1);
    assert(log_field(line,"req",&value) && value>=1);
    assert(log_field(line,"pages",&value) && value>=1);

    open_no_mem=true; logs_reset();
    assert(download(&config)==TASKS_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"done",&value) && value==1);
    assert(log_field(line,"stage",&value) && value==5); /* HEADERS, no open */
    assert(log_field(line,"http",&value) && value>=1);
    assert(log_field(line,"open",&value) && value==0);
    assert(log_field(line,"resp",&value) && value==0);
    open_no_mem=false;

    init_fail=true; logs_reset();
    assert(download(&config)==TASKS_FAILED);
    line=log_first("DLDIAG ");
    assert(log_field(line,"done",&value) && value==1);
    assert(log_field(line,"stage",&value) && value==3); /* CACHE */
    assert(log_field(line,"http",&value) && value==0);
    assert(log_field(line,"open",&value) && value==0);
    init_fail=false;
}

/* A: a successful HTTP open is reached even when the following budget/cancel
   check rejects. Functional cancel/cleanup/owner/cache behavior is unchanged. */
static void boundary_a_control_open(void)
{
    reset_harness(); (void)prepare_cache(); logs_reset();
    boundary_preempt_cancel=false; boundary_cancel_after_open=false;
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    assert(download(&config)==TASKS_READY);
    long long value=0; const char *line=log_first("DLDIAG ");
    assert(log_field(line,"open",&value) && value==1);
}
static void boundary_a_open_success_then_cancel(void)
{
    reset_harness(); uint32_t generation=prepare_cache(); logs_reset();
    boundary_preempt_cancel=false; boundary_cancel_after_open=true;
    unsigned prior_calls=calls,prior_cleans=cleans,prior_takes=takes,prior_gives=gives;
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    assert(download(&config)==TASKS_CANCELLED);
    assert(!boundary_cancel_after_open && calls==prior_calls+1 && cleans==prior_cleans+1);
    assert(takes-prior_takes==gives-prior_gives && !network_live);
    assert(s_cache.valid && s_cache.active.generation==generation);
    long long value=0; const char *line=log_first("DLDIAG ");
    assert(log_field(line,"result",&value) && value==9); /* CANCELLED */
    assert(log_field(line,"open",&value) && value==1);
    assert(log_field(line,"resp",&value) && value==0);
}

/* B: the cancel flag publication and its diagnostic timestamp are one bounded
   critical operation, so a permitted switch cannot finalize first. */
static void boundary_b_case(bool preempt)
{
    reset_harness(); (void)prepare_voice_ready(); logs_reset();
    boundary_cancel_after_open=false; boundary_preempt_cancel=false; boundary_preemptions=0;
    unsigned old_reads=audio_reads,old_posts=voice_posts;
    assert(test_critical_depth==0);
    assert(suite_tasks_voice(1));
    boundary_preempt_cancel=preempt;
    (void)suite_tasks_voice(4);
    boundary_preempt_cancel=false;
    voice_process();
    assert(test_critical_depth==0 && s_view.voice_state==VOICE_READY);
    assert(audio_reads==old_reads && voice_posts==old_posts && !s_creation.magic && !nvs_creation.magic);
    const char *line=diag_final_line(); long long value=0;
    assert(log_field(line,"result",&value) && value==2); /* CANCEL_PREWORKER */
    assert(log_field(line,"d_cancel",&value) && value>=0);
}
static void boundary_b_control(void) { boundary_b_case(false); }
static void boundary_b_preempt(void) { boundary_b_case(true); }

static void legacy_groups(void)
{
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    memset(memory,255,sizeof(memory)); s_partition=&partition;
    task_io_t io={.ctx=&partition,.read=io_read,.write=io_write,.erase=io_erase};
    assert(!task_cache_open(&s_cache,io));
    online=false; assert(download(&config)==TASKS_OFFLINE && !calls && !takes);
    online=true; tracker.state=SUITE_TIME_UNSYNCED;
    assert(download(&config)==TASKS_TIME_REQUIRED && !calls && !takes);
    tracker.state=SUITE_TIME_SYNCED; net_available=false;
    assert(download(&config)==TASKS_FAILED && !calls && !gives);
    net_available=true; init_fail=true;
    assert(download(&config)==TASKS_FAILED && !calls && gives==1);
    init_fail=false; assert(download(&config)==TASKS_READY && s_cache.valid);
    unsigned generation=s_cache.active.generation;
    assert(calls==cleans);
    /* A TLS allocation failure after client creation must release the client
       and network owner, retaining the last committed snapshot. */
    open_no_mem=true;
    unsigned given=gives;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    assert(!last_read_bytes && calls==cleans && gives==given+1 && !network_live);
    open_no_mem=false;
    /* A transport read error after partial receipt must never activate a
       partial snapshot or retain a live TLS client/network lock. */
    read_fail=true; given=gives;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    assert(last_read_bytes && calls==cleans && gives==given+1 && !network_live);
    read_fail=false;
    http_status=302; assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    http_status=200; complete=false;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    complete=true; overflow_rx=true;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    overflow_rx=false; cancel_rx=true;
    assert(download(&config)==TASKS_CANCELLED && s_cache.active.generation==generation);
    cancel_rx=false; atomic_store(&s_cancel,false);
    chunked_overflow=true;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    assert(last_read_bytes==TASK_PAGE_MAX+512);
    chunked_overflow=false; cancel_cleanup=true;
    assert(download(&config)==TASKS_CANCELLED && s_cache.active.generation==generation);
    cancel_cleanup=false; atomic_store(&s_cancel,false); expire_rx=true;
    assert(download(&config)==TASKS_FAILED && s_cache.active.generation==generation);
    expire_rx=false;
    http_status=401; assert(download(&config)==TASKS_AUTH_FAILED);
    unsigned previous=calls; http_status=200;
    assert(download(&config)==TASKS_AUTH_FAILED && calls==previous);
    assert(calls==cleans && !network_live);
    assert(note_end("abcd",0)==4);
    char note[780]; memset(note,'a',767); strcpy(note+767,"\xe4\xb8\xad" "END");
    assert(note_end(note,0)==767 && note_end(note,767)==773);
    puts("task HTTPS / offline / time / cancellation / auth halt / cleanup: PASS");
    s_auth_halted=false; atomic_store(&s_cancel,false); s_completion_configured=true;
    task_journal_io_t journal_io={.load=journal_load,.save=journal_save,.erase=journal_erase,
        .sequence=journal_sequence,.save_sequence=journal_save_sequence};
    assert(task_journal_open(&s_journal,journal_io));
    char id[65],opid[33],response[512]; memset(id,'a',64); id[64]=0; memset(opid,'1',32); opid[32]=0;
    online=false; assert(task_journal_add(&s_journal,id,"\"base\"",opid,0));
    assert(!sync_one(false) && s_journal.entries[0].state==TASK_OP_LOCAL);
    task_journal_t recovered; assert(task_journal_open(&recovered,journal_io));
    assert(!strcmp(recovered.entries[0].operation_id,opid));
    snprintf(response,sizeof(response),"{\"schema\":1,\"operation_id\":\"%s\",\"task_id\":\"%s\",\"state\":\"SERVER_DURABLE\",\"error\":null,\"updated_at\":1792368000}",opid,id);
    payload=response; online=true; http_status=202;
    assert(sync_one(false) && s_journal.entries[0].state==TASK_OP_SERVER);
    assert(strstr(posted,"abababababab") && strstr(posted,"\"retry\":false"));
    previous=nvs_writes; monotonic=s_op_due[0]+1;
    assert(!sync_one(false) && nvs_writes==previous); /* Polling does not wear NVS. */
    snprintf(response,sizeof(response),"{\"schema\":1,\"operation_id\":\"%s\",\"task_id\":\"%s\",\"state\":\"SUCCEEDED\",\"error\":null,\"updated_at\":1792368000}",opid,id);
    monotonic=s_op_due[0]+1;
    assert(sync_one(false) && s_journal.entries[0].state==TASK_OP_SUCCEEDED);
    assert(atomic_load(&s_auto_refresh) && !atomic_load(&s_refresh));
    assert(task_journal_open(&recovered,journal_io) && recovered.entries[0].state==TASK_OP_SUCCEEDED);
    clean_completed(); assert(!task_journal_count(&s_journal) && nvs_erases==1);
    assert(task_journal_add(&s_journal,id,"\"base\"",opid,0));
    complete=false; memset(s_op_due,0,sizeof(s_op_due)); memset(s_op_attempts,0,sizeof(s_op_attempts));
    for (unsigned attempt=0;attempt<5;attempt++) { monotonic=s_op_due[0]+1; (void)sync_one(false); }
    assert(s_journal.entries[0].state==TASK_OP_PAUSED);
    previous=calls; monotonic=s_op_due[0]+1; assert(!sync_one(false) && calls==previous);
    complete=true;
    assert(sync_one(true) && s_journal.entries[0].state==TASK_OP_SUCCEEDED);
    assert(strstr(posted,"\"retry\":true"));
    clean_completed(); assert(!task_journal_count(&s_journal));
    assert(task_journal_add(&s_journal,id,"\"base\"",opid,0));
    update_view();
    assert(s_view.count==1 && s_view.retained_count==1 && s_view.request_only);
    assert(!strcmp(s_view.task_id,id) && s_view.operation==TASK_OP_LOCAL && !s_view.completion_allowed);
    assert(task_journal_update(&s_journal,0,TASK_OP_NOT_FOUND)); update_view();
    assert(s_view.operation==TASK_OP_NOT_FOUND && s_view.retained_count==1);
    s_command.kind=2; strcpy(s_command.task_id,id); s_command.etag[0]=0;
    process_command(); update_view();
    assert(s_view.count==0 && s_view.retained_count==0 && !task_journal_count(&s_journal));
    assert(calls==cleans && !network_live);
    puts("completion offline NVS / stable replay / separate token / receipt / retry pause / polling wear: PASS");
    puts("source-deleted completion remains visible and explicitly discardable: PASS");
    unsigned prior_takes=takes,prior_gives=gives;
    voice_script=true; http_status=200; s_task=(TaskHandle_t)1; voice_load();
    assert(s_voice_journal_ready && s_voice_state==VOICE_READY);
    assert(suite_tasks_voice(1)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && voice_posts==1 && audio_released && !nvs_creation.magic);
    assert(!strcmp(s_voice_title,"买牛奶"));
    nvs_fail=true; assert(suite_tasks_voice(3)); voice_process();
    assert(s_voice_state==VOICE_PREVIEW && !s_creation.magic);
    nvs_fail=false; assert(suite_tasks_voice(3)); voice_process();
    assert(s_voice_state==VOICE_PENDING && task_creation_valid(&nvs_creation));
    task_creation_t saved=s_creation; memset(&s_creation,0,sizeof(s_creation)); voice_load();
    assert(!strcmp(saved.operation_id,s_creation.operation_id));
    online=false; unsigned sent=calls; voice_sync(false); assert(calls==sent);
    online=true; voice_sync(false); assert(s_creation.state==TASK_OP_SUCCEEDED && nvs_creation.state==TASK_OP_SUCCEEDED);
    assert(atomic_load(&s_auto_refresh));
    voice_load(); assert(s_voice_state==VOICE_SUCCEEDED);
    assert(suite_tasks_voice(6)); voice_process(); assert(!nvs_creation.magic && s_voice_state==VOICE_READY);
    voice_cancel_capture=true;
    assert(suite_tasks_voice(1)); voice_process(); assert(s_voice_state==VOICE_READY && voice_posts==1);
    /* Leaving before the worker starts must not turn the microphone on. */
    unsigned old_reads=audio_reads;
    assert(suite_tasks_voice(1)); (void)suite_tasks_voice(4); voice_process(); assert(audio_reads==old_reads);
    assert(calls==cleans && takes-prior_takes==gives-prior_gives);
    memset(&nvs_creation,255,sizeof(nvs_creation)); voice_load();
    assert(!s_voice_journal_ready && s_voice_state==VOICE_STORAGE_FAILED && !s_creation.magic && !s_view.voice_summary[0]);
    assert(nvs_creation.magic==UINT32_MAX); /* Corrupt input is preserved on Flash. */
    s_voice_cache_blocked=true;
    assert(download(&config)==TASKS_STORAGE_FAILED);
    s_voice_cache_blocked=false;
    puts("voice worker capture/cancel/cleanup / preview-before-create / NVS failure / reboot replay: PASS");
}

static uint32_t slot_crc(unsigned slot)
{
    uint32_t crc=~0U;
    unsigned char chunk[512];
    for (uint32_t at=0;at<TASK_SLOT_SIZE;at+=sizeof(chunk)) {
        assert(io_read((void *)&partition,slot*TASK_SLOT_SIZE+at,chunk,sizeof(chunk)));
        crc=task_crc32(crc,chunk,sizeof(chunk));
    }
    return crc;
}

/* The shared scratch lease is the actual outer owner used by Tasks refresh and
   voice. A held translation capture must block refresh before any N/erase, and
   cancellation/cleanup must release S without changing the active generation. */
static void translation_lease_blocks_refresh_and_releases(void)
{
    reset_harness();
    credentials_t config={.origin="https://example.test",.token="abcdefghijklmnopqrstuvwxyz_0123456789"};
    uint32_t generation=prepare_cache();
    int active=s_cache.slot;
    unsigned inactive=(unsigned)(1-active);
    unsigned marker_at=inactive*TASK_SLOT_SIZE;
    suite_audio_lease_t *lease=NULL;
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION,0,&lease)==ESP_OK);
    assert(suite_audio_job_stage_begin(lease,0x50000)==ESP_OK);
    unsigned char pcm[SUITE_AUDIO_CHUNK_MAX];
    memset(pcm,0x6c,sizeof(pcm));
    assert(suite_audio_job_append(lease,pcm,sizeof(pcm))==ESP_OK);
    uint32_t base=suite_audio_job_base(lease);
    unsigned char audio_before[SUITE_AUDIO_CHUNK_MAX];
    memcpy(audio_before,memory+base+44,sizeof(audio_before));
    assert(*(const uint32_t *)(memory+marker_at)==0x31445541U);
    uint32_t active_crc=slot_crc((unsigned)active);
    unsigned prior_calls=calls;
    virtual_clock_step=1000;
    assert(download(&config)==TASKS_FAILED);
    virtual_clock_step=0;
    assert(calls==prior_calls); /* no HTTPS and no inactive-slot erase */
    assert(s_cache.valid && s_cache.active.generation==generation && s_cache.slot==active);
    assert(slot_crc((unsigned)active)==active_crc); /* active cache byte-identical */
    assert(!memcmp(memory+base+44,audio_before,sizeof(audio_before)));
    assert(*(const uint32_t *)(memory+marker_at)==0x31445541U);
    assert(suite_audio_job_release(lease)==ESP_OK);
    assert(!suite_audio_job_held());
    assert(*(const uint32_t *)(memory+marker_at)!=0x31445541U);
    assert(download(&config)==TASKS_READY);
    assert(s_cache.valid && s_cache.active.generation==generation+1);

    /* Explicit cancellation still cleans and releases S for another owner. */
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION,0,&lease)==ESP_OK);
    assert(suite_audio_job_stage_begin(lease,0x50000)==ESP_OK);
    assert(suite_audio_job_append(lease,pcm,sizeof(pcm))==ESP_OK);
    suite_audio_job_cancel(lease);
    assert(suite_audio_job_is_cancelled(lease));
    assert(suite_audio_job_abort(lease)==ESP_OK);
    assert(!suite_audio_job_held());
    assert(!suite_audio_job_slot_quarantined((unsigned)(1-s_cache.slot)));
    assert(suite_audio_job_acquire(SUITE_AUDIO_OWNER_TASKS_VOICE,0,&lease)==ESP_OK);
    assert(suite_audio_job_release(lease)==ESP_OK);
    puts("shared S lease blocks refresh before N/erase, cancel releases, active generation retained: PASS");
}

static int run_group(const char *name,void (*body)(void))
{
    group_name=name; group_failure[0]=0;
    printf("[ RUN  ] %s\n",name); fflush(stdout);
    if (setjmp(group_jump)) { printf("[ FAIL ] %s (%s)\n",name,group_failure); return 1; }
    body();
    printf("[ PASS ] %s\n",name); return 0;
}

int main(void)
{
    static const struct { const char *name; void (*body)(void); } groups[]={
        {"existing download/completion/voice regressions",legacy_groups},
        {"shared scratch lease blocks refresh and cancels cleanly",translation_lease_blocks_refresh_and_releases},
        {"R1 slow transcription within budget",r1_slow_transcription_within_budget},
        {"R1 transcription deadline fails closed",r1_deadline_exceeded_fails_closed},
        {"R1 cancel after complete response",r1_cancel_after_complete_response},
        {"R2 CREATION_TARGET_REQUIRED is discardable",r2_creation_target_required_is_discardable},
        {"R2 unknown 400 retains intent",r2_unknown_400_retains_intent},
        {"R2 QUEUE_FULL retains intent",r2_queue_full_retains_intent},
        {"R2 lost response retains intent",r2_lost_response_retains_intent},
        {"R2 duplicate error key retains intent",r2_duplicate_error_key_retains_intent},
        {"R2 trailing garbage retains intent",r2_trailing_garbage_retains_intent},
        {"R2 missing error retains intent",r2_missing_error_retains_intent},
        {"R2 non-string error retains intent",r2_non_string_error_retains_intent},
        {"R2 HTTP 503 same string retains intent",r2_503_same_string_retains_intent},
        {"R2 escaped NUL value retains intent",r2_escaped_nul_value_retains_intent},
        {"R2 escaped NUL key retains intent",r2_escaped_nul_key_retains_intent},
        {"R2 raw NUL tail retains intent",r2_raw_nul_body_retains_intent},
        {"R3 preflight write-token 401",r3_preflight_write_token_401},
        {"R3 preflight CREATION_TARGET_REQUIRED",r3_preflight_creation_target_required},
        {"R3 transcription SPEECH_AUTH_FAILED",r3_transcription_auth_failed},
        {"R3 transcription SPEECH_BUDGET_EXCEEDED",r3_transcription_budget},
        {"R3 transcription SPEECH_UNAVAILABLE",r3_transcription_unavailable},
        {"R3 transcription SPEECH_TIMEOUT",r3_transcription_timeout},
        {"R3 transcription unknown 400",r3_transcription_unknown},
        {"R3 transcription malformed body",r3_transcription_malformed},
        {"R4 missing journal after command 1",r4_journal_not_ready_command1},
        {"R4 cancel before worker keeps unready failure",r4_cancel_before_worker_keeps_unready_failure},
        {"R4 command 4 does not mask missing journal",r4_command4_does_not_mask_missing_journal},
        {"R4 blocked cache stays terminal",r4_cache_blocked_stays_terminal},
        {"R4 SUCCEEDED receipt replays same ID after save failure",r4_succeeded_save_failure_replays_same_id},
        {"DIAG pre-worker cancel has no stale markers",diag_preworker_cancel_no_stale},
        {"DIAG stop request and capture end are separate",diag_capture_stop_separates_request},
        {"DIAG capture cancel records release and cleanup",diag_capture_cancel_release_cleanup},
        {"DIAG cleanup failure is terminal",diag_cleanup_failure_is_terminal},
        {"DIAG download early returns invent no HTTP",diag_download_early_no_http},
        {"DIAG terminal reentry keeps both finals",diag_terminal_reentry_keeps_both_finals},
        {"DIAG preview cancel request is recorded",diag_preview_cancel_request_recorded},
        {"DIAG timing boundaries and upload phases",diag_timing_boundaries_and_upload},
        {"DIAG release failure is explicit",diag_release_failure_is_explicit},
        {"DIAG credentials unconfigured is numeric",diag_credentials_unconfigured_numeric},
        {"DIAG download stage and open boundaries",diag_download_stage_and_boundaries},
        {"BOUNDARY A control successful open",boundary_a_control_open},
        {"BOUNDARY A open success then cancel",boundary_a_open_success_then_cancel},
        {"BOUNDARY B control cancel timestamp",boundary_b_control},
        {"BOUNDARY B preempt flag before timestamp",boundary_b_preempt},
    };
    unsigned failed=0;
    for (unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);i++) {
        reset_harness();
        failed+=(unsigned)run_group(groups[i].name,groups[i].body);
    }
    printf("%u of %u groups failed\n",failed,(unsigned)(sizeof(groups)/sizeof(groups[0])));
    return failed ? 1 : 0;
}
