#include "suite_tasks.h"
#include "suite_audio_job.h"
#include "suite_tasks_core.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_net.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static task_cache_t s_cache;
static const esp_partition_t *s_partition;
static suite_tasks_view_t s_view;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static atomic_bool s_refresh, s_auto_refresh, s_cancel, s_busy;
static atomic_uint s_selection;
static unsigned s_index, s_note_page;
static bool s_detail;
static bool s_auth_halted;
static task_journal_t s_journal;
static bool s_completion_configured;
static bool s_voice_cache_blocked;
static char s_notice[96];
static struct { unsigned kind; char task_id[65],etag[257]; } s_command;
static unsigned s_op_attempts[TASK_OP_CAPACITY];
static int64_t s_op_due[TASK_OP_CAPACITY];
static bool s_op_retry[TASK_OP_CAPACITY];
static unsigned s_orphan_slots[TASK_OP_CAPACITY],s_orphan_count;
static uint32_t s_orphan_generation,s_orphan_sequence;
static unsigned s_orphan_mask;
static suite_tasks_state_t s_halted_state=TASKS_AUTH_FAILED;
static const char *TAG="suite_tasks";

static bool io_read(void *ctx,uint32_t p,void *buf,size_t n)
{ return esp_partition_read(ctx,p,buf,n)==ESP_OK; }
static bool io_write(void *ctx,uint32_t p,const void *buf,size_t n)
{ return esp_partition_write(ctx,p,buf,n)==ESP_OK; }
static bool io_erase(void *ctx,uint32_t p,size_t n)
{ return esp_partition_erase_range(ctx,p,n)==ESP_OK; }
static bool trusted(void)
{ return suite_time_is_trusted(suite_connection_time(),suite_connection_now()); }

static void state(suite_tasks_state_t value)
{
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    s_view.state=value; s_view.serial++;
    xSemaphoreGive(s_mutex);
}

typedef struct { char origin[193],token[129],write_token[129]; } credentials_t;
static bool credentials(credentials_t *out)
{
    nvs_sec_cfg_t key={0}; nvs_handle_t h;
    const esp_partition_t *keys=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,"taskkeys");
    bool ok=false;
    if (!keys || nvs_flash_read_security_cfg(keys,&key)!=ESP_OK) goto done;
    if (nvs_flash_secure_init_partition("taskcfg",&key)!=ESP_OK) goto done;
    if (nvs_open_from_partition("taskcfg","tasks",NVS_READONLY,&h)!=ESP_OK) goto done;
    size_t n=sizeof(out->origin);
    esp_err_t err=nvs_get_str(h,"origin",out->origin,&n);
    n=sizeof(out->token);
    if (err==ESP_OK) err=nvs_get_str(h,"token",out->token,&n);
    uint32_t schema=0;
    if (err==ESP_OK) err=nvs_get_u32(h,"schema",&schema);
    n=sizeof(out->write_token);
    if (nvs_get_str(h,"write_token",out->write_token,&n)!=ESP_OK ||
        !task_token_valid(out->write_token) || !strcmp(out->write_token,out->token)) out->write_token[0]=0;
    nvs_close(h);
    ok=err==ESP_OK && schema==1 && task_origin_valid(out->origin) && task_token_valid(out->token);
done:
    mbedtls_platform_zeroize(&key,sizeof(key));
    return ok;
}

static int journal_load(void *ctx,unsigned slot,task_operation_t *op)
{
    (void)ctx; nvs_handle_t h; char key[8]; snprintf(key,sizeof(key),"op%u",slot);
    if (nvs_open_from_partition("taskcfg","taskops",NVS_READWRITE,&h)!=ESP_OK) return -1;
    size_t bytes=sizeof(*op); esp_err_t result=nvs_get_blob(h,key,op,&bytes); nvs_close(h);
    return result==ESP_ERR_NVS_NOT_FOUND ? 0 : result==ESP_OK && bytes==sizeof(*op) ? 1 : -1;
}
static bool journal_save(void *ctx,unsigned slot,const task_operation_t *op)
{
    (void)ctx; nvs_handle_t h; char key[8]; snprintf(key,sizeof(key),"op%u",slot);
    if (nvs_open_from_partition("taskcfg","taskops",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t result=nvs_set_blob(h,key,op,sizeof(*op));
    if (result==ESP_OK) result=nvs_commit(h);
    nvs_close(h); return result==ESP_OK;
}
static bool journal_erase(void *ctx,unsigned slot)
{
    (void)ctx; nvs_handle_t h; char key[8]; snprintf(key,sizeof(key),"op%u",slot);
    if (nvs_open_from_partition("taskcfg","taskops",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t result=nvs_erase_key(h,key);
    if (result==ESP_ERR_NVS_NOT_FOUND) result=ESP_OK;
    if (result==ESP_OK) result=nvs_commit(h);
    nvs_close(h); return result==ESP_OK;
}
static int journal_sequence(void *ctx,uint32_t *sequence)
{
    (void)ctx; nvs_handle_t h;
    if (nvs_open_from_partition("taskcfg","taskops",NVS_READWRITE,&h)!=ESP_OK) return -1;
    esp_err_t result=nvs_get_u32(h,"sequence",sequence); nvs_close(h);
    if (result==ESP_ERR_NVS_NOT_FOUND) { *sequence=0; return 0; }
    return result==ESP_OK ? 1 : -1;
}
static bool journal_save_sequence(void *ctx,uint32_t sequence)
{
    (void)ctx; nvs_handle_t h;
    if (nvs_open_from_partition("taskcfg","taskops",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t result=nvs_set_u32(h,"sequence",sequence);
    if (result==ESP_OK) result=nvs_commit(h);
    nvs_close(h); return result==ESP_OK;
}

typedef struct {
    uint32_t base;
    size_t bytes;
    int64_t deadline;
    bool failed;
} receive_t;
static bool receive_bytes(receive_t *rx,const void *bytes,size_t n)
{
    if (atomic_load(&s_cancel) || esp_timer_get_time()>=rx->deadline) return false;
    if (rx->failed || n>TASK_PAGE_MAX-rx->bytes ||
        !io_write((void *)s_partition,rx->base+rx->bytes,bytes,n)) {
        rx->failed=true; return false;
    }
    rx->bytes+=n; return true;
}
/* Versioned, numeric-only download evidence. Early returns keep http/req/pages
   at zero: no client or transfer is invented. Heap samples are observations,
   not instantaneous worst cases; capability sets overlap and are not additive. */
#define DL_DIAG_VERSION 2U
#define DL_DIAG_TLS_LOG_MAX 4U
enum { DL_DIAG_READY=1,DL_DIAG_STORAGE,DL_DIAG_OFFLINE,DL_DIAG_TIME,DL_DIAG_AUTH,
       DL_DIAG_NET,DL_DIAG_ALLOC,DL_DIAG_TRANSPORT,DL_DIAG_CANCELLED,DL_DIAG_FAILED,
       DL_DIAG_SOURCE_AUTH,DL_DIAG_SCRATCH };
enum { DL_DIAG_ENTRY=1,DL_DIAG_NET_TAKEN,DL_DIAG_CACHE,DL_DIAG_CLIENT,DL_DIAG_HEADERS,
       DL_DIAG_TRANSFER,DL_DIAG_PARSE,DL_DIAG_COMMIT };
static void dl_diag_sample(int64_t *ifree,int64_t *ilarge,int64_t *dfree,int64_t *dlarge,int64_t *stack)
{
    *ifree=(int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    *ilarge=(int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    *dfree=(int64_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    *dlarge=(int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    *stack=(int64_t)uxTaskGetStackHighWaterMark(NULL);
}
static unsigned dl_diag_result(suite_tasks_state_t state)
{
    if (state==TASKS_READY) return DL_DIAG_READY;
    if (state==TASKS_STORAGE_FAILED) return DL_DIAG_STORAGE;
    if (state==TASKS_OFFLINE) return DL_DIAG_OFFLINE;
    if (state==TASKS_TIME_REQUIRED) return DL_DIAG_TIME;
    if (state==TASKS_AUTH_FAILED) return DL_DIAG_AUTH;
    if (state==TASKS_SOURCE_AUTH_FAILED) return DL_DIAG_SOURCE_AUTH;
    if (state==TASKS_CANCELLED) return DL_DIAG_CANCELLED;
    return DL_DIAG_FAILED;
}
static void dl_diag_return(suite_tasks_state_t state,unsigned code,unsigned stage,unsigned done,
                           unsigned http,unsigned opens,unsigned resp,unsigned req,
                           unsigned pages,int64_t started)
{
    /* Boundary first; resource sampling is observer overhead. */
    int64_t now=esp_timer_get_time();
    int64_t ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
    dl_diag_sample(&ifree,&ilarge,&dfree,&dlarge,&stack);
    ESP_LOGI(TAG,"DLDIAG v=%u result=%u stage=%u done=%u http=%u open=%u resp=%u "
        "req=%u pages=%u dur_us=%lld ifree_b=%lld ilarge_b=%lld dfree_b=%lld "
        "dlarge_b=%lld stack_b=%lld",
        DL_DIAG_VERSION,code ? code : dl_diag_result(state),stage,done,http,opens,resp,
        req,pages,(long long)(now-started),(long long)ifree,(long long)ilarge,
        (long long)dfree,(long long)dlarge,(long long)stack);
}
static void dl_diag_tls_alive(unsigned page)
{
    if (page>=DL_DIAG_TLS_LOG_MAX) return;
    int64_t ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
    dl_diag_sample(&ifree,&ilarge,&dfree,&dlarge,&stack);
    ESP_LOGI(TAG,"DLTLS v=%u page=%u ifree_b=%lld ilarge_b=%lld dfree_b=%lld dlarge_b=%lld stack_b=%lld",
        DL_DIAG_VERSION,page,(long long)ifree,(long long)ilarge,(long long)dfree,
        (long long)dlarge,(long long)stack);
}
static bool time_limit(esp_http_client_handle_t client,int64_t deadline)
{
    int64_t ms=(deadline-esp_timer_get_time())/1000;
    if (ms<=0 || atomic_load(&s_cancel)) return false;
    if (ms>10000) ms=10000;
    return esp_http_client_set_timeout_ms(client,(int)ms)==ESP_OK;
}
static bool transfer(esp_http_client_handle_t client,receive_t *rx,int *status,unsigned page,
                     bool *opened,bool *responded)
{
    /* Use the explicit read API: IDF ignores an ON_DATA callback's return value,
       so callback rejection alone cannot bound a hostile/never-ending body. */
    *opened=false; *responded=false;
    if (!time_limit(client,rx->deadline)) return false;
    if (esp_http_client_open(client,0)!=ESP_OK) return false;
    *opened=true; /* A successful open is reached even if the next check fails. */
    if (!time_limit(client,rx->deadline)) return false;
    int64_t length=esp_http_client_fetch_headers(client);
    *status=esp_http_client_get_status_code(client);
    *responded=true;
    dl_diag_tls_alive(page); /* Client and TLS are alive at this checkpoint. */
    if (length<0 || length>TASK_PAGE_MAX || *status!=200) return false;
    char chunk[512];
    for (;;) {
        if (!time_limit(client,rx->deadline)) return false;
        int n=esp_http_client_read(client,chunk,sizeof(chunk));
        if (n<0) return false;
        if (!n) return esp_http_client_is_complete_data_received(client);
        if (!receive_bytes(rx,chunk,(size_t)n)) return false;
    }
}

static bool download_cancelled(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_cancel);
}
static suite_tasks_state_t download(credentials_t *config)
{
    int64_t started=esp_timer_get_time();
    unsigned http=0,opens=0,resp=0,req=0,pages=0,diag_stage=DL_DIAG_ENTRY;
    if (s_voice_cache_blocked) {
        dl_diag_return(TASKS_STORAGE_FAILED,0,diag_stage,0,0,0,0,0,0,started);
        return TASKS_STORAGE_FAILED;
    }
    if (suite_connection_state()!=SUITE_CONN_ONLINE) {
        dl_diag_return(TASKS_OFFLINE,0,diag_stage,0,0,0,0,0,0,started);
        return TASKS_OFFLINE;
    }
    if (!trusted()) {
        dl_diag_return(TASKS_TIME_REQUIRED,0,diag_stage,0,0,0,0,0,0,started);
        return TASKS_TIME_REQUIRED;
    }
    if (s_auth_halted) {
        dl_diag_return(s_halted_state,0,diag_stage,0,0,0,0,0,0,started);
        return s_halted_state;
    }
    /* S covers the authoritative cache and its generation; N is acquired
       inside S and released before S. A translation capture lease therefore
       prevents this refresh from erasing the slot it read from. */
    suite_audio_lease_t *lease=NULL;
    if (suite_audio_job_acquire_ex(SUITE_AUDIO_OWNER_TASKS_REFRESH,1000,
            download_cancelled,NULL,&lease)!=ESP_OK) {
        dl_diag_return(TASKS_FAILED,DL_DIAG_SCRATCH,diag_stage,0,0,0,0,0,0,started);
        return TASKS_FAILED;
    }
    if (!suite_net_take(1000)) {
        (void)suite_audio_job_release(lease);
        dl_diag_return(TASKS_FAILED,DL_DIAG_NET,diag_stage,0,0,0,0,0,0,started);
        return TASKS_FAILED;
    }
    diag_stage=DL_DIAG_NET_TAKEN;
    const unsigned caps=MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    int64_t ifree=0,ilarge=0,dfree=0,dlarge=0,stack=0;
    dl_diag_sample(&ifree,&ilarge,&dfree,&dlarge,&stack);
    ESP_LOGI(TAG,"Before download: free=%lld, largest=%lld, dma_free=%lld, dma_largest=%lld",
        (long long)ifree,(long long)ilarge,(long long)dfree,(long long)dlarge);
    suite_tasks_state_t result=TASKS_FAILED;
    int64_t deadline=esp_timer_get_time()+90000000;
    if (!task_cache_begin(&s_cache)) { result=TASKS_STORAGE_FAILED; goto done; }
    diag_stage=DL_DIAG_CACHE;
    unsigned offset=0;
    for (unsigned page=0;page<TASK_MAX_ITEMS+1;page++) {
        if (atomic_load(&s_cancel)) { result=TASKS_CANCELLED; break; }
        if (esp_timer_get_time()>deadline || !trusted()) break;
        uint32_t scratch=s_cache.staging*TASK_SLOT_SIZE+TASK_SCRATCH;
        if (!io_erase((void *)s_partition,scratch,TASK_PAGE_MAX)) { result=TASKS_STORAGE_FAILED; break; }
        char url[320],auth[140];
        if (!offset) snprintf(url,sizeof(url),"%s/suite/v1/tasks/snapshot",config->origin);
        else snprintf(url,sizeof(url),"%s/suite/v1/tasks/snapshot?revision=%s&offset=%u",
            config->origin,s_cache.pending.revision,offset);
        snprintf(auth,sizeof(auth),"Bearer %s",config->token);
        receive_t rx={.base=scratch,.deadline=deadline};
        esp_http_client_config_t cfg={.url=url,
            .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=10000,
            .buffer_size=1024,.buffer_size_tx=512,.disable_auto_redirect=true};
        esp_http_client_handle_t client=esp_http_client_init(&cfg);
        if (!client) { mbedtls_platform_zeroize(auth,sizeof(auth)); break; }
        http++; diag_stage=DL_DIAG_CLIENT;
        esp_err_t header=esp_http_client_set_header(client,"Authorization",auth);
        if (header==ESP_OK) header=esp_http_client_set_header(client,"Accept","application/json");
        mbedtls_platform_zeroize(auth,sizeof(auth));
        if (header!=ESP_OK) { esp_http_client_cleanup(client); break; }
        int status=0;
        req++; diag_stage=DL_DIAG_HEADERS;
        bool opened=false,responded=false;
        bool received=transfer(client,&rx,&status,page,&opened,&responded);
        opens+=(opened?1U:0U); resp+=(responded?1U:0U); pages++;
        esp_http_client_cleanup(client); /* Release TLS before parsing any records. */
        if (opened) diag_stage=DL_DIAG_TRANSFER;
        if (atomic_load(&s_cancel)) { result=TASKS_CANCELLED; break; }
        if (status==401 || status==403) {
            s_auth_halted=true; s_halted_state=TASKS_AUTH_FAILED; result=s_halted_state; break;
        }
        if (!received || status!=200 || rx.failed || !rx.bytes) break;
        task_record_t *work=malloc(sizeof(*work));
        if (!work) break;
        task_page_t parsed;
        diag_stage=DL_DIAG_PARSE;
        bool valid=task_cache_page(&s_cache,rx.bytes,&parsed,work);
        free(work);
        if (!valid) break;
        if (parsed.last) {
            if (atomic_load(&s_cancel)) { result=TASKS_CANCELLED; break; }
            if (!trusted() || esp_timer_get_time()>deadline) break;
            diag_stage=DL_DIAG_COMMIT;
            if (task_cache_commit(&s_cache,suite_connection_now())) {
                result=s_cache.active.auth ? TASKS_SOURCE_AUTH_FAILED : TASKS_READY;
                if (s_cache.active.auth) { s_auth_halted=true; s_halted_state=result; }
            } else result=TASKS_STORAGE_FAILED;
            break;
        }
        offset=parsed.next;
    }
done:
    suite_net_give();
    (void)suite_audio_job_release(lease);
    dl_diag_return(result,0,diag_stage,1,http,opens,resp,req,pages,started);
    ESP_LOGI(TAG,"Refresh result=%d, cached=%u, stack remaining=%u, free=%u, largest=%u",result,
        s_cache.valid ? (unsigned)s_cache.active.count : 0U,(unsigned)uxTaskGetStackHighWaterMark(NULL),
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps));
    return result;
}

/* Pages are cut at UTF-8 boundaries. Each full note is reachable without a huge
   LVGL label, while source truncation is indicated separately. */
static size_t note_end(const char *s,size_t start)
{
    size_t end=start+768, length=strlen(s);
    if (end>=length) return length;
    while (end>start && ((unsigned char)s[end]&0xc0)==0x80) end--;
    return end;
}

static bool post_operation(credentials_t *config,const task_operation_t *op,bool retry,task_op_state_t *result)
{
    if (!config->write_token[0] || !trusted() || suite_connection_state()!=SUITE_CONN_ONLINE || !suite_net_take(1000)) return false;
    if (op->state==TASK_OP_LOCAL || retry) ESP_LOGI(TAG,"Before completion: free=%u, largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    struct { char request[1400],response[1025]; } *buffer=calloc(1,sizeof(*buffer));
    esp_http_client_handle_t client=NULL; bool received=false,ok=false; int status=0; size_t used=0;
    unsigned char digest[32]; char hex[65],url[256],auth[144];
    if (!buffer || !task_operation_json(op,buffer->request,sizeof(buffer->request),NULL,false) ||
        mbedtls_sha256((const unsigned char *)buffer->request,strlen(buffer->request),digest,0)!=0) goto done;
    for (unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
    if (!task_operation_json(op,buffer->request,sizeof(buffer->request),hex,retry)) goto done;
    snprintf(url,sizeof(url),"%s/suite/v1/task-completions",config->origin);
    snprintf(auth,sizeof(auth),"Bearer %s",config->write_token);
    esp_http_client_config_t settings={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=10000,.buffer_size=512,.buffer_size_tx=1024,.disable_auto_redirect=true};
    client=esp_http_client_init(&settings);
    if (!client || esp_http_client_set_method(client,HTTP_METHOD_POST)!=ESP_OK ||
        esp_http_client_set_header(client,"Authorization",auth)!=ESP_OK ||
        esp_http_client_set_header(client,"Content-Type","application/json")!=ESP_OK) goto done;
    size_t bytes=strlen(buffer->request);
    if (esp_http_client_open(client,(int)bytes)!=ESP_OK) goto done;
    int64_t deadline=esp_timer_get_time()+30000000;
    for (size_t sent=0;sent<bytes;) {
        if (esp_timer_get_time()>=deadline) goto done;
        int n=esp_http_client_write(client,buffer->request+sent,(int)(bytes-sent));
        if (n<=0) goto done;
        sent+=(size_t)n;
    }
    int64_t length=esp_http_client_fetch_headers(client);
    status=esp_http_client_get_status_code(client);
    if (status!=200 && status!=202 && status!=400 && status!=404 && status!=409) goto done;
    if (length<0 || length>1024) goto done;
    while (used<1025 && esp_timer_get_time()<deadline) {
        int n=esp_http_client_read(client,buffer->response+used,(int)(1025-used));
        if (n<0) goto done;
        if (!n) { received=esp_http_client_is_complete_data_received(client); break; }
        used+=(size_t)n;
    }
done:
    if (client) esp_http_client_cleanup(client);
    suite_net_give();
    /* Decode after TLS releases its buffers, as with snapshot downloads. */
    if (received && used<=1024) {
        if (status==200 || status==202) ok=task_operation_receipt(buffer->response,used,op,result);
        else ok=task_operation_error(buffer->response,used,result);
    }
    else if (status==401 || status==403) { *result=TASK_OP_AUTH; ok=true; }
    else if (status==409) { *result=TASK_OP_CONFLICT; ok=true; }
    else if (status==404) { *result=TASK_OP_NOT_FOUND; ok=true; }
    else if (status==400) { *result=TASK_OP_PAUSED; ok=true; }
    if (buffer) { mbedtls_platform_zeroize(buffer,sizeof(*buffer)); free(buffer); }
    return ok;
}

static void process_command(void)
{
    unsigned kind; char id[65],etag[257];
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    kind=s_command.kind; strcpy(id,s_command.task_id); strcpy(etag,s_command.etag); s_command.kind=0;
    xSemaphoreGive(s_mutex);
    if (!kind) return;
    bool ok=false;
    if (kind==2) {
        int slot=task_journal_find(&s_journal,id);
        if (slot>=0) {
            unsigned st=s_journal.entries[slot].state;
            if (st==TASK_OP_CONFLICT || st==TASK_OP_READONLY || st==TASK_OP_NOT_FOUND)
                ok=task_journal_discard(&s_journal,(unsigned)slot);
        }
        snprintf(s_notice,sizeof(s_notice),"%s",ok ? "旧请求已放弃，请刷新" : "无法放弃该请求");
        return;
    }
    task_record_t *record=malloc(sizeof(*record));
    if (record && s_cache.valid && s_completion_configured && s_journal.ready) {
        for (unsigned i=0;i<s_cache.active.count;i++) {
            if (!task_cache_record(&s_cache,i,record)) break;
            if (!strcmp(record->task_id,id) && !strcmp(record->etag,etag) && record->completable) {
                unsigned char random[16]; char opid[33]; esp_fill_random(random,sizeof(random));
                for (unsigned j=0;j<16;j++) snprintf(opid+2*j,3,"%02x",random[j]);
                ok=task_journal_add(&s_journal,id,etag,opid,trusted() ? suite_connection_now() : 0);
                if (ok) {
                    int slot=task_journal_find(&s_journal,id);
                    s_op_due[slot]=0; s_op_attempts[slot]=0; s_op_retry[slot]=false;
                }
                break;
            }
        }
    }
    free(record);
    snprintf(s_notice,sizeof(s_notice),"%s",ok ? "已保存完成意图，待同步" :
        task_journal_count(&s_journal)>=TASK_OP_CAPACITY ? "完成队列已满，未保存" : "完成操作未保存，请重试");
}

static void clean_completed(void)
{
    if (!s_journal.ready || !s_cache.valid) return;
    task_record_t *record=malloc(sizeof(*record)); if (!record) return;
    for (unsigned slot=0;slot<TASK_OP_CAPACITY;slot++) {
        task_operation_t *op=&s_journal.entries[slot];
        if (!op->magic || op->state!=TASK_OP_SUCCEEDED) continue;
        bool old=false,failed=false;
        for (unsigned i=0;i<s_cache.active.count;i++) {
            if (!task_cache_record(&s_cache,i,record)) { failed=true; break; }
            if (!strcmp(record->task_id,op->task_id) && !strcmp(record->etag,op->etag)) { old=true; break; }
        }
        if (!old && !failed) (void)task_journal_discard(&s_journal,slot);
    }
    free(record);
}

static bool sync_one(bool retry)
{
    if (retry) {
        memset(s_op_attempts,0,sizeof(s_op_attempts)); memset(s_op_due,0,sizeof(s_op_due));
        for (unsigned i=0;i<TASK_OP_CAPACITY;i++) s_op_retry[i]=true;
    }
    if (!s_journal.ready || !s_completion_configured || suite_connection_state()!=SUITE_CONN_ONLINE || !trusted()) return false;
    int64_t now=esp_timer_get_time();
    for (unsigned slot=0;slot<TASK_OP_CAPACITY;slot++) {
        task_operation_t *op=&s_journal.entries[slot];
        if (!op->magic || op->state==TASK_OP_SUCCEEDED || op->state==TASK_OP_CONFLICT ||
            op->state==TASK_OP_READONLY || op->state==TASK_OP_NOT_FOUND ||
            ((op->state==TASK_OP_AUTH || op->state==TASK_OP_PAUSED || op->state==TASK_OP_FAILED) && !s_op_retry[slot]) || s_op_due[slot]>now) continue;
        credentials_t config={0}; task_op_state_t next=TASK_OP_LOCAL;
        bool received=credentials(&config) && post_operation(&config,op,s_op_retry[slot],&next);
        s_op_retry[slot]=false;
        mbedtls_platform_zeroize(&config,sizeof(config));
        now=esp_timer_get_time(); bool changed=false;
        if (received) {
            changed=op->state!=(unsigned)next;
            if (!task_journal_update(&s_journal,slot,next)) snprintf(s_notice,sizeof(s_notice),"同步结果保存异常");
            else if (next==TASK_OP_SUCCEEDED) {
                snprintf(s_notice,sizeof(s_notice),"已在 Nextcloud 完成");
                atomic_store(&s_auto_refresh,true); xTaskNotifyGive(s_task);
            } else if (next==TASK_OP_CONFLICT) snprintf(s_notice,sizeof(s_notice),"完成冲突，请查看详情");
            else if (next==TASK_OP_AUTH) snprintf(s_notice,sizeof(s_notice),"完成认证失败，待修复");
            else if (next==TASK_OP_READONLY) snprintf(s_notice,sizeof(s_notice),"该待办暂不支持完成");
            else if (next==TASK_OP_NOT_FOUND) snprintf(s_notice,sizeof(s_notice),"远端待办已不存在");
            else if (next==TASK_OP_PAUSED) snprintf(s_notice,sizeof(s_notice),"同步已暂停，刷新重试");
            s_op_attempts[slot]=0; s_op_due[slot]=now+8000000;
            if (changed) ESP_LOGI(TAG,"Completion result=%u, pending=%u, stack remaining=%u, free=%u, largest=%u",
                (unsigned)next,task_journal_count(&s_journal),(unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        } else {
            unsigned attempt=++s_op_attempts[slot];
            const unsigned delays[]={2,5,15,60,300};
            s_op_due[slot]=now+(int64_t)delays[attempt<5 ? attempt-1 : 4]*1000000;
            if (attempt>=5) (void)task_journal_update(&s_journal,slot,TASK_OP_PAUSED);
            changed=attempt>=5;
        }
        return changed;
    }
    return false;
}
/* Keep orphaned intents reachable after source deletion/selection changes.
   Scan once per cache generation or journal membership change, not on scrolling. */
static bool refresh_orphans(task_record_t *record)
{
    unsigned mask=0;
    if (s_journal.ready) for (unsigned i=0;i<TASK_OP_CAPACITY;i++)
        if (s_journal.entries[i].magic && s_journal.entries[i].state!=TASK_OP_SUCCEEDED) mask|=1U<<i;
    uint32_t generation=s_cache.valid ? s_cache.active.generation : 0;
    if (generation==s_orphan_generation && s_journal.sequence==s_orphan_sequence && mask==s_orphan_mask) return true;
    unsigned missing=mask;
    if (s_cache.valid && missing) for (unsigned i=0;i<s_cache.active.count;i++) {
        if (!task_cache_record(&s_cache,i,record)) { s_orphan_count=0; return false; }
        int slot=task_journal_find(&s_journal,record->task_id);
        if (slot>=0) missing&=~(1U<<(unsigned)slot);
        if (!missing) break;
    }
    s_orphan_count=0;
    for (unsigned i=0;i<TASK_OP_CAPACITY;i++) if (missing&(1U<<i)) s_orphan_slots[s_orphan_count++]=i;
    s_orphan_generation=generation; s_orphan_sequence=s_journal.sequence; s_orphan_mask=mask;
    return true;
}
static bool display_record(unsigned index,task_record_t *record)
{
    unsigned cached=s_cache.valid ? s_cache.active.count : 0;
    if (index<cached) return task_cache_record(&s_cache,index,record);
    index-=cached; if (index>=s_orphan_count) return false;
    const task_operation_t *op=&s_journal.entries[s_orphan_slots[index]];
    memset(record,0,sizeof(*record));
    strcpy(record->task_id,op->task_id); strcpy(record->etag,op->etag);
    strcpy(record->summary,"保留的完成请求"); strcpy(record->status,"REQUEST"); strcpy(record->due_kind,"none");
    strcpy(record->note,"原任务已不在当前快照，完成请求仍保留。\n请查看同步状态。冲突、只读或不存在的请求可在操作菜单确认放弃；进行中的请求请等待同步。");
    return true;
}
static void update_view(void)
{
    task_record_t *r=malloc(sizeof(*r));
    if (!r) { state(TASKS_FAILED); return; }
    bool records_ok=refresh_orphans(r);
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    unsigned selection=atomic_load(&s_selection);
    s_index=selection&255U; s_detail=(selection&256U)!=0; s_note_page=selection>>9;
    s_view.cached=s_cache.valid;
    s_view.count=(s_cache.valid ? s_cache.active.count : 0)+s_orphan_count;
    s_view.retained_count=s_orphan_count;
    if (!records_ok) s_view.state=TASKS_STORAGE_FAILED;
    if (s_index>=s_view.count) s_index=s_view.count ? s_view.count-1 : 0;
    s_view.selected=s_index; s_view.detail=s_detail;
    s_view.server_at=s_cache.valid ? s_cache.active.server_at : 0;
    s_view.device_at=s_cache.valid ? s_cache.active.device_at : 0;
    s_view.stale=task_cache_stale(&s_cache,suite_connection_now(),trusted());
    s_view.completion_configured=s_completion_configured; s_view.journal_ready=s_journal.ready;
    s_view.completion_allowed=false; s_view.request_only=false; s_view.task_id[0]=0; s_view.etag[0]=0;
    s_view.operation=TASK_OP_NONE; s_view.pending_count=0;
    if (s_journal.ready) for (unsigned i=0;i<TASK_OP_CAPACITY;i++)
        if (s_journal.entries[i].magic && s_journal.entries[i].state!=TASK_OP_SUCCEEDED) s_view.pending_count++;
    snprintf(s_view.notice,sizeof(s_view.notice),"%s",s_notice);
    memset(s_view.titles,0,sizeof(s_view.titles)); s_view.note[0]=0;
    memset(s_view.row_operations,0,sizeof(s_view.row_operations));
    unsigned first=s_index/3*3;
    for (unsigned i=0;i<3 && first+i<s_view.count;i++) {
        if (!display_record(first+i,r)) { s_view.state=TASKS_STORAGE_FAILED; break; }
        memcpy(s_view.titles[i],r->summary,sizeof(r->summary));
        int slot=task_journal_find(&s_journal,r->task_id);
        s_view.row_operations[i]=slot>=0 ? (task_op_state_t)s_journal.entries[slot].state : TASK_OP_NONE;
    }
    if (s_view.count && display_record(s_index,r)) {
        s_view.request_only=s_index>=(s_cache.valid ? s_cache.active.count : 0);
        snprintf(s_view.status,sizeof(s_view.status),"%s",r->status);
        strcpy(s_view.task_id,r->task_id); strcpy(s_view.etag,r->etag);
        int slot=task_journal_find(&s_journal,r->task_id);
        if (slot>=0) s_view.operation=(task_op_state_t)s_journal.entries[slot].state;
        s_view.completion_allowed=r->completable && s_completion_configured && s_journal.ready && slot<0 && task_journal_count(&s_journal)<TASK_OP_CAPACITY;
        const char *kind=!strcmp(r->due_kind,"date") ? "日期" :
            !strcmp(r->due_kind,"floating") ? "当地时间，无时区" :
            !strcmp(r->due_kind,"zoned") ? "指定时区" : "UTC";
        if (!strcmp(r->due_kind,"none")) snprintf(s_view.due,sizeof(s_view.due),"无到期时间");
        else snprintf(s_view.due,sizeof(s_view.due),"到期（%s）\n%s\n原值：%s%s%s",kind,
            r->due_display[0] ? r->due_display : "--",r->due_value,
            r->due_zone[0] ? "\n时区：" : "",r->due_zone);
        memcpy(s_view.collection,r->collection,sizeof(r->collection));
        s_view.truncated=r->truncated; s_view.recurring=r->recurring; s_view.parent=r->parent;
        size_t start=0,end=0; unsigned pages=0;
        do { end=note_end(r->note,start); pages++; start=end; } while (r->note[end]);
        if (s_note_page>=pages) s_note_page=pages-1;
        start=0;
        for (unsigned p=0;p<s_note_page;p++) start=note_end(r->note,start);
        end=note_end(r->note,start);
        memcpy(s_view.note,r->note+start,end-start); s_view.note[end-start]=0;
        s_view.note_page=s_note_page; s_view.note_pages=pages;
    }
    s_view.serial++;
    xSemaphoreGive(s_mutex); free(r);
}

#include "suite_tasks_voice_impl.h"

static void worker(void *arg)
{
    (void)arg;
    s_partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x40,"taskcache");
    if (!s_partition || s_partition->size!=2*TASK_SLOT_SIZE) { state(TASKS_STORAGE_FAILED); goto idle; }
    task_io_t io={.ctx=(void *)s_partition,.read=io_read,.write=io_write,.erase=io_erase};
    (void)task_cache_open(&s_cache,io);
    if (suite_audio_job_bind(&s_cache,io)!=ESP_OK) { state(TASKS_STORAGE_FAILED); goto idle; }
    credentials_t initial={0};
    bool configured=credentials(&initial);
    s_completion_configured=configured && initial.write_token[0];
    if (configured) {
        task_journal_io_t journal_io={.load=journal_load,.save=journal_save,.erase=journal_erase,
            .sequence=journal_sequence,.save_sequence=journal_save_sequence};
        (void)task_journal_open(&s_journal,journal_io);
    }
    state(configured ? TASKS_READY : TASKS_UNCONFIGURED);
    mbedtls_platform_zeroize(&initial,sizeof(initial));
    voice_load();
    update_view();
    bool was_online=suite_connection_state()==SUITE_CONN_ONLINE;
    for (;;) {
        bool changed=ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000))!=0;
        process_command();
        voice_process();
        bool manual=atomic_exchange(&s_refresh,false);
        bool automatic=atomic_exchange(&s_auto_refresh,false);
        if (manual || automatic) {
            if (manual) s_notice[0]=0;
            atomic_store(&s_busy,true);
            credentials_t config={0};
            if (!credentials(&config)) state(TASKS_UNCONFIGURED);
            else {
                state(TASKS_REFRESHING);
                suite_tasks_state_t downloaded=download(&config);
                state(downloaded);
                if (downloaded==TASKS_READY) clean_completed();
            }
            mbedtls_platform_zeroize(&config,sizeof(config));
            atomic_store(&s_busy,false);
        }
        bool online=suite_connection_state()==SUITE_CONN_ONLINE;
        changed=sync_one(manual || (online && !was_online)) || changed || manual || automatic;
        voice_sync(manual || (online && !was_online));
        was_online=online;
        if (changed) update_view();
    }
idle:
    for (;;) (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
}

esp_err_t suite_tasks_start(void)
{
    if (s_task) return ESP_OK;
    esp_err_t audio=suite_audio_job_init();
    if (audio!=ESP_OK) return audio;
    s_mutex=xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_view.state=TASKS_LOADING;
    /* Completion adds credential/journal frames and bounded JSON serialization
       to the old download path (which measured ~1.5 KiB remaining at 6 KiB). */
    if (xTaskCreate(worker,"suite_tasks",8192,NULL,3,&s_task)!=pdPASS) {
        vSemaphoreDelete(s_mutex); s_mutex=NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void suite_tasks_select(unsigned i,bool detail,unsigned page)
{
    if (!s_task) return;
    if (i>=TASK_MAX_ITEMS+TASK_OP_CAPACITY) i=TASK_MAX_ITEMS+TASK_OP_CAPACITY-1;
    if (page>16) page=16;
    atomic_store(&s_selection,i | (detail ? 256U : 0U) | (page<<9));
    xTaskNotifyGive(s_task);
}
void suite_tasks_refresh(void)
{
    if (!s_task) return;
    if (atomic_load(&s_busy)) return;
    /* Coalesce repeated refreshes. Auth failures remain halted until reboot and
       a repaired/replaced physical USB configuration; no blind auth retry. */
    if (atomic_exchange(&s_refresh,true)) return;
    atomic_store(&s_cancel,false); xTaskNotifyGive(s_task);
}
void suite_tasks_cancel(void) { atomic_store(&s_cancel,true); }
static bool submit_command(unsigned kind,const char *id,const char *etag)
{
    if (!s_task || !task_id_valid(id) || (kind==1 && !task_etag_valid(etag)) ||
        xSemaphoreTake(s_mutex,0)!=pdTRUE) return false;
    bool ok=!s_command.kind;
    if (ok) {
        s_command.kind=kind; strcpy(s_command.task_id,id);
        if (etag) strcpy(s_command.etag,etag); else s_command.etag[0]=0;
        snprintf(s_view.notice,sizeof(s_view.notice),"正在保存，请稍候"); s_view.serial++;
    }
    xSemaphoreGive(s_mutex);
    if (ok) xTaskNotifyGive(s_task);
    return ok;
}
bool suite_tasks_complete(const char *id,const char *etag) { return submit_command(1,id,etag); }
bool suite_tasks_discard_conflict(const char *id) { return submit_command(2,id,NULL); }
const suite_tasks_view_t *suite_tasks_view_acquire(void)
{
    static const suite_tasks_view_t unavailable={.state=TASKS_UNAVAILABLE};
    if (!s_mutex) return &unavailable;
    if (xSemaphoreTake(s_mutex,0)!=pdTRUE) return NULL;
    int64_t now=suite_connection_now();
    s_view.stale=s_view.stale || !trusted() || now<s_view.server_at || now-s_view.server_at>=86400;
    return &s_view;
}
void suite_tasks_view_release(void) { if (s_mutex) xSemaphoreGive(s_mutex); }
