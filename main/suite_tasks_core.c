#include "suite_tasks_core.h"
#include "suite_task_ops.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define MAGIC 0x31544b53U

uint32_t task_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}

/* A bounded pull JSON reader: no whole-page RAM allocation, including on host. */
typedef struct {
    const uint8_t *memory;
    const task_io_t *io;
    uint32_t base, pos, end, block_at;
    size_t block_len;
    uint8_t block[128];
    bool bad;
} json_t;

static int peek(json_t *j)
{
    if (j->bad || j->pos >= j->end) return -1;
    if (j->memory) return j->memory[j->pos];
    if (!j->block_len || j->pos < j->block_at ||
        j->pos >= j->block_at + j->block_len) {
        j->block_at = j->pos;
        j->block_len = j->end - j->pos;
        if (j->block_len > sizeof(j->block)) j->block_len = sizeof(j->block);
        if (!j->io->read(j->io->ctx, j->base + j->pos, j->block, j->block_len)) {
            j->bad = true;
            return -1;
        }
    }
    return j->block[j->pos - j->block_at];
}

static int get(json_t *j) { int c = peek(j); if (c >= 0) j->pos++; return c; }
static void ws(json_t *j)
{
    int c;
    while ((c = peek(j)) == ' ' || c == '\n' || c == '\r' || c == '\t') j->pos++;
}
static bool take(json_t *j, int c) { ws(j); return get(j) == c; }
static bool literal(json_t *j, const char *s)
{
    ws(j);
    while (*s) if (get(j) != *s++) return false;
    return true;
}
static int hex4(json_t *j)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = get(j), d = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

static bool string(json_t *j, char *out, size_t cap)
{
    if (!take(j, '"')) return false;
    size_t n = 0;
    for (;;) {
        int c = get(j);
        if (c == '"') { if (out) out[n] = 0; return true; }
        if (c < 32) return false;
        uint32_t cp = (unsigned)c;
        if (c == '\\') {
            c = get(j);
            if (c == 'u') {
                int u = hex4(j);
                if (u < 0) return false;
                cp = (unsigned)u;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (get(j) != '\\' || get(j) != 'u') return false;
                    int lo = hex4(j);
                    if (lo < 0xdc00 || lo > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
            } else {
                switch (c) {
                case '"': case '\\': case '/': cp = (unsigned)c; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                default: return false;
                }
            }
        } else if (c >= 128) {
            unsigned more;
            uint32_t min;
            if (c >= 0xc2 && c <= 0xdf) { more = 1; cp &= 31; min = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { more = 2; cp &= 15; min = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { more = 3; cp &= 7; min = 0x10000; }
            else return false;
            while (more--) {
                int d = get(j);
                if (d < 0x80 || d > 0xbf) return false;
                cp = (cp << 6) | (d & 63);
            }
            if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        }
        if (!cp) return false; /* Never permit hidden suffixes after a NUL. */
        uint8_t b[4]; size_t len;
        if (cp < 0x80) { b[0] = cp; len = 1; }
        else if (cp < 0x800) { b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 63); len = 2; }
        else if (cp < 0x10000) {
            b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); len = 3;
        } else {
            b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63);
            b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); len = 4;
        }
        if (cap && n + len >= cap) return false;
        if (out) memcpy(out + n, b, len);
        n += len;
    }
}

static bool number(json_t *j, int64_t *out)
{
    ws(j); int c = peek(j); int64_t v = 0;
    if (c < '0' || c > '9') return false;
    bool zero = c == '0';
    do {
        if (v > (INT64_MAX - (c - '0')) / 10) return false;
        v = v * 10 + get(j) - '0'; c = peek(j);
        if (zero && c >= '0' && c <= '9') return false;
    } while (c >= '0' && c <= '9');
    *out = v; return true;
}
static bool boolean(json_t *j, bool *v)
{
    ws(j); *v = peek(j) == 't'; return literal(j, *v ? "true" : "false");
}
static bool skip(json_t *j, unsigned depth)
{
    if (depth > 16) return false;
    ws(j); int c = peek(j);
    if (c == '"') return string(j, NULL, 0);
    if (c == '{' || c == '[') {
        get(j); ws(j);
        int close = c == '{' ? '}' : ']';
        if (peek(j) == close) { get(j); return true; }
        do {
            if (c == '{' && (!string(j, NULL, 0) || !take(j, ':'))) return false;
            if (!skip(j, depth + 1)) return false;
            ws(j); if (peek(j) != ',') break; get(j);
        } while (true);
        return take(j, close);
    }
    if (c == 'n') return literal(j, "null");
    if (c == 't' || c == 'f') { bool b; return boolean(j, &b); }
    int64_t n; return number(j, &n);
}

static bool nullable_string(json_t *j, char *out, size_t cap)
{
    ws(j); if (peek(j) == 'n') { out[0] = 0; return literal(j, "null"); }
    return string(j, out, cap);
}
static bool due(json_t *j, task_record_t *r)
{
    if (!take(j, '{')) return false;
    unsigned fields = 0;
    do {
        char key[40]; if (!string(j, key, sizeof(key)) || !take(j, ':')) return false;
        unsigned flag = !strcmp(key,"kind") ? 1 : !strcmp(key,"value") ? 2 :
            !strcmp(key,"display") ? 4 : !strcmp(key,"tzid") ? 8 : 0;
        if (fields & flag) return false;
        fields |= flag;
        if (flag == 1) { if (!string(j,r->due_kind,sizeof(r->due_kind))) return false; }
        else if (flag == 2) { if (!nullable_string(j,r->due_value,sizeof(r->due_value))) return false; }
        else if (flag == 4) { if (!nullable_string(j,r->due_display,sizeof(r->due_display))) return false; }
        else if (flag == 8) { if (!string(j,r->due_zone,sizeof(r->due_zone))) return false; }
        else if (!skip(j, 0)) return false;
        ws(j); if (peek(j) != ',') break; get(j);
    } while (true);
    if (!take(j,'}') || (fields & 7) != 7) return false;
    if (!strcmp(r->due_kind,"none")) return !r->due_value[0] && !r->due_display[0];
    if (!strcmp(r->due_kind,"zoned") && !r->due_zone[0]) return false;
    return r->due_value[0] && r->due_display[0] &&
        (!strcmp(r->due_kind,"date") || !strcmp(r->due_kind,"zoned") ||
         !strcmp(r->due_kind,"utc") || !strcmp(r->due_kind,"floating"));
}

static bool record(json_t *j, task_record_t *r, const char *revision, int64_t server_at)
{
    memset(r, 0, sizeof(*r));
    if (!take(j, '{')) return false;
    unsigned seen = 0;
    do {
        char k[40]; if (!string(j,k,sizeof(k)) || !take(j,':')) return false;
        const char *keys[] = {"schema","summary","note","status","collection_id","due",
            "note_truncated","recurring","has_parent","read_only","snapshot_revision","downloaded_at",
            "account_id","uid","etag","href","recurrence_id","task_id","completion_allowed"};
        unsigned i;
        for (i=0; i<sizeof(keys)/sizeof(keys[0]); i++) if (!strcmp(k,keys[i])) break;
        unsigned flag = i < 19 ? 1U << i : 0;
        if (seen & flag) return false;
        seen |= flag;
        int64_t n; bool b; char small[33];
        switch (i) {
        case 0: if (!number(j,&n) || n != 1) return false; break;
        case 1: if (!string(j,r->summary,sizeof(r->summary))) return false; break;
        case 2: if (!string(j,r->note,sizeof(r->note))) return false; break;
        case 3: if (!string(j,r->status,sizeof(r->status))) return false; break;
        case 4: if (!string(j,r->collection,sizeof(r->collection)) || !r->collection[0]) return false; break;
        case 5: if (!due(j,r)) return false; break;
        case 6: if (!boolean(j,&r->truncated)) return false; break;
        case 7: if (!boolean(j,&r->recurring)) return false; break;
        case 8: if (!boolean(j,&r->parent)) return false; break;
        case 9: if (!boolean(j,&b) || !b) return false; break;
        case 10: if (!string(j,small,sizeof(small)) || (revision && strcmp(small,revision))) return false; break;
        case 11: if (!number(j,&n) || (server_at && n != server_at)) return false; break;
        case 14: if (!string(j,r->etag,sizeof(r->etag)) || !r->etag[0]) return false; break;
        case 17: if (!string(j,r->task_id,sizeof(r->task_id)) || !task_id_valid(r->task_id)) return false; break;
        case 18: if (!boolean(j,&r->completable)) return false; break;
        case 12: case 13: case 15: {
            /* Identity/locator are retained verbatim in Flash, never reconstructed. */
            uint32_t start = j->pos;
            size_t limit=i==12 ? 65 : i==13 ? 1025 : i==14 ? 257 : 2049;
            if (!string(j,NULL,limit) || j->pos - start < 3) return false;
            break;
        }
        default: if (!skip(j,0)) return false;
        }
        ws(j); if (peek(j) != ',') break; get(j);
    } while (true);
    return take(j,'}') && (seen & 0x1ffff) == 0x1ffff &&
        (!r->completable || (task_id_valid(r->task_id) && task_etag_valid(r->etag) && !r->recurring && !r->parent &&
            (!strcmp(r->status,"NEEDS-ACTION") || !strcmp(r->status,"IN-PROCESS")))) &&
        (!strcmp(r->status,"NEEDS-ACTION") || !strcmp(r->status,"IN-PROCESS") ||
         !strcmp(r->status,"COMPLETED") || !strcmp(r->status,"CANCELLED"));
}

bool task_record_parse(const void *json, size_t length, task_record_t *out)
{
    if (!json || !out || !length || length > TASK_RECORD_MAX) return false;
    json_t j = {.memory=json,.end=(uint32_t)length};
    if (!record(&j,out,NULL,0)) return false;
    ws(&j); return !j.bad && j.pos == j.end;
}

bool task_operation_receipt(const char *json,size_t bytes,const task_operation_t *op,task_op_state_t *state)
{
    if (!json || bytes>1024 || !bytes || !op || !state) return false;
    json_t j={.memory=(const uint8_t *)json,.end=(uint32_t)bytes}; unsigned seen=0; task_op_state_t result=TASK_OP_NONE;
    if (!take(&j,'{')) return false;
    do {
        char key[32],value[65]; int64_t n;
        if (!string(&j,key,sizeof(key)) || !take(&j,':')) return false;
        const char *keys[]={"schema","operation_id","task_id","state","error","updated_at"};
        unsigned i; for (i=0;i<6;i++) if (!strcmp(key,keys[i])) break;
        if (i==6 || (seen&(1U<<i))) return false;
        seen|=1U<<i;
        switch (i) {
        case 0: if (!number(&j,&n) || n!=1) return false; break;
        case 1: if (!string(&j,value,sizeof(value)) || strcmp(value,op->operation_id)) return false; break;
        case 2: if (!string(&j,value,sizeof(value)) || strcmp(value,op->task_id)) return false; break;
        case 3:
            if (!string(&j,value,sizeof(value))) return false;
            if (!strcmp(value,"SERVER_DURABLE") || !strcmp(value,"REMOTE_APPLYING") || !strcmp(value,"RETRY_WAIT")) result=TASK_OP_SERVER;
            else if (!strcmp(value,"SUCCEEDED")) result=TASK_OP_SUCCEEDED;
            else if (!strcmp(value,"CONFLICT")) result=TASK_OP_CONFLICT;
            else if (!strcmp(value,"AUTH_FAILED")) result=TASK_OP_AUTH;
            else if (!strcmp(value,"READ_ONLY")) result=TASK_OP_READONLY;
            else if (!strcmp(value,"NOT_FOUND")) result=TASK_OP_NOT_FOUND;
            else if (!strcmp(value,"RETRY_PAUSED")) result=TASK_OP_PAUSED;
            else if (!strcmp(value,"FAILED")) result=TASK_OP_FAILED;
            else return false;
            break;
        case 4:
            ws(&j);
            if (peek(&j)=='n') { if (!literal(&j,"null")) return false; }
            else if (!string(&j,value,sizeof(value))) return false;
            break;
        case 5: if (!number(&j,&n) || n<0) return false; break;
        }
        ws(&j); if (peek(&j)!=',') break; get(&j);
    } while (true);
    if (!take(&j,'}')) return false;
    ws(&j); if (j.bad || j.pos!=j.end || seen!=63 || result==TASK_OP_NONE) return false;
    *state=result; return true;
}

bool task_operation_error(const char *json,size_t bytes,task_op_state_t *state)
{
    if (!json || !state || !bytes || bytes>1024) return false;
    json_t j={.memory=(const uint8_t *)json,.end=(uint32_t)bytes};
    char key[16],error[40];
    if (!take(&j,'{') || !string(&j,key,sizeof(key)) || strcmp(key,"error") ||
        !take(&j,':') || !string(&j,error,sizeof(error)) || !take(&j,'}')) return false;
    ws(&j); if (j.bad || j.pos!=j.end) return false;
    if (!strcmp(error,"READ_ONLY")) *state=TASK_OP_READONLY;
    else if (!strcmp(error,"NOT_FOUND")) *state=TASK_OP_NOT_FOUND;
    else if (!strcmp(error,"CONFLICT") || !strcmp(error,"OPERATION_CONFLICT")) *state=TASK_OP_CONFLICT;
    else *state=TASK_OP_PAUSED;
    return true;
}

static uint32_t header_crc(const task_header_t *h)
{ return task_crc32(~0U,h,offsetof(task_header_t,header_crc)); }

static bool validate(task_cache_t *c, int slot, task_header_t *h)
{
    uint32_t base = slot * TASK_SLOT_SIZE;
    if (!c->io.read(c->io.ctx,base,h,sizeof(*h)) || h->magic != MAGIC || h->version != 1 ||
        !h->generation || h->count > TASK_MAX_ITEMS || h->bytes > TASK_CACHE_MAX ||
        h->header_crc != header_crc(h) || h->revision[32] || !h->revision[0] ||
        h->device_at <= 0 || h->server_at <= 0) return false;
    uint32_t pos = TASK_DATA_START;
    for (unsigned i=0; i<h->count; i++) {
        if (h->offsets[i] != pos || !h->lengths[i] || h->lengths[i] > TASK_RECORD_MAX) return false;
        pos += h->lengths[i];
    }
    if (pos != TASK_DATA_START + h->bytes) return false;
    uint8_t buf[256]; uint32_t crc = ~0U;
    for (uint32_t p=0; p<h->bytes;) {
        size_t n = h->bytes-p; if (n>sizeof(buf)) n=sizeof(buf);
        if (!c->io.read(c->io.ctx,base+TASK_DATA_START+p,buf,n)) return false;
        crc=task_crc32(crc,buf,n); p+=n;
    }
    return crc == h->crc;
}

bool task_cache_open(task_cache_t *c, task_io_t io)
{
    if (!c || !io.read || !io.write || !io.erase) return false;
    memset(c,0,sizeof(*c)); c->io=io; c->slot=-1; c->staging=-1;
    bool a=validate(c,0,&c->active), b=validate(c,1,&c->pending);
    if (b && (!a || c->pending.generation > c->active.generation)) {
        c->active=c->pending; c->slot=1;
    } else if (a) c->slot=0;
    c->valid = c->slot >= 0;
    return c->valid;
}

bool task_cache_begin(task_cache_t *c)
{
    c->staging=c->slot == 0 ? 1 : 0; c->failed=true;
    memset(&c->pending,0,sizeof(c->pending));
    if (c->valid && c->active.generation == UINT32_MAX) return false;
    if (!c->io.erase(c->io.ctx,c->staging*TASK_SLOT_SIZE,TASK_SLOT_SIZE)) return false;
    c->pending.magic=MAGIC; c->pending.version=1;
    c->pending.generation=c->valid ? c->active.generation+1 : 1;
    c->running_crc=~0U; c->failed=false;
    return true;
}

static bool append(task_cache_t *c, uint32_t start, size_t n)
{
    task_header_t *h=&c->pending;
    if (h->count >= TASK_MAX_ITEMS || !n || n>TASK_RECORD_MAX || h->bytes+n>TASK_CACHE_MAX) return false;
    uint32_t base=c->staging*TASK_SLOT_SIZE, off=TASK_DATA_START+h->bytes;
    uint8_t buf[256];
    for (size_t p=0;p<n;) {
        size_t m=n-p; if (m>sizeof(buf)) m=sizeof(buf);
        if (!c->io.read(c->io.ctx,base+TASK_SCRATCH+start+p,buf,m) ||
            !c->io.write(c->io.ctx,base+off+p,buf,m)) return false;
        c->running_crc=task_crc32(c->running_crc,buf,m); p+=m;
    }
    h->offsets[h->count]=off; h->lengths[h->count++]=(uint16_t)n; h->bytes+=n;
    return true;
}

static bool revision_valid(const char *s)
{
    if (strlen(s)!=32) return false;
    for (unsigned i=0;i<32;i++) if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return false;
    return true;
}

bool task_cache_page(task_cache_t *c, size_t bytes, task_page_t *p, task_record_t *r)
{
    if (c->failed || c->staging<0 || !bytes || bytes>TASK_PAGE_MAX) return false;
    c->failed=true; memset(p,0,sizeof(*p));
    json_t j={.io=&c->io,.base=c->staging*TASK_SLOT_SIZE+TASK_SCRATCH,.end=(uint32_t)bytes};
    if (!take(&j,'{')) return false;
    unsigned seen=0; uint32_t item_start=0,item_end=0;
    do {
        char k[40]; if (!string(&j,k,sizeof(k)) || !take(&j,':')) return false;
        const char *keys[]={"schema","snapshot_revision","selection_revision","downloaded_at","state",
            "read_only","total","offset","next_offset","items"};
        unsigned i; for (i=0;i<10;i++) if (!strcmp(k,keys[i])) break;
        unsigned flag=i<10 ? 1U<<i : 0; if (seen&flag) return false; seen|=flag;
        int64_t n=0; bool b;
        switch (i) {
        case 0: if (!number(&j,&n) || n!=1) return false; break;
        case 1: if (!string(&j,p->revision,sizeof(p->revision)) || !revision_valid(p->revision)) return false; break;
        case 2: if (!number(&j,&p->selection)) return false; break;
        case 3: if (!number(&j,&p->server_at) || !p->server_at) return false; break;
        case 4: if (!string(&j,p->state,sizeof(p->state))) return false; break;
        case 5: if (!boolean(&j,&b) || !b) return false; break;
        case 6: case 7: case 8:
            ws(&j);
            if (i==8 && peek(&j)=='n') { if (!literal(&j,"null")) return false; p->last=true; break; }
            if (!number(&j,&n) || n>TASK_MAX_ITEMS) return false;
            if (i==6) p->total=(uint32_t)n;
            else if (i==7) p->offset=(uint32_t)n;
            else p->next=(uint32_t)n;
            break;
        case 9: item_start=j.pos; if (!skip(&j,0)) return false; item_end=j.pos; break;
        default: if (!skip(&j,0)) return false;
        }
        ws(&j); if (peek(&j)!=',') break; get(&j);
    } while (true);
    if (!take(&j,'}')) return false;
    ws(&j);
    if (j.bad || j.pos!=j.end || seen!=1023 || p->offset!=c->pending.count ||
        (strcmp(p->state,"READY") && strcmp(p->state,"EMPTY") && strcmp(p->state,"STALE") && strcmp(p->state,"AUTH_FAILED"))) return false;
    task_header_t *h=&c->pending;
    if (!p->offset) {
        memcpy(h->revision,p->revision,sizeof(h->revision)); h->selection=p->selection; h->server_at=p->server_at;
        h->reserved[0]=(uint8_t)p->total;
    } else if (strcmp(h->revision,p->revision) || h->selection!=p->selection || h->server_at!=p->server_at ||
               h->reserved[0]!=p->total) return false;
    h->stale |= !strcmp(p->state,"STALE"); h->auth |= !strcmp(p->state,"AUTH_FAILED");
    j.pos=item_start; j.end=item_end; j.block_len=0;
    if (!take(&j,'[')) return false;
    ws(&j);
    if (peek(&j)!=']') do {
        ws(&j); uint32_t start=j.pos;
        if (!record(&j,r,h->revision,h->server_at) || !append(c,start,j.pos-start)) return false;
        p->count++;
        ws(&j); if (peek(&j)!=',') break; get(&j);
    } while (true);
    if (!take(&j,']')) return false;
    ws(&j);
    if (j.bad || j.pos!=j.end || h->count>p->total ||
        (p->last ? h->count!=p->total : (!p->count || p->next!=h->count || p->next>=p->total)) ||
        (!strcmp(p->state,"EMPTY") && p->total)) return false;
    h->reserved[1]=p->last;
    c->failed=false; return true;
}

bool task_cache_commit(task_cache_t *c, int64_t device_at)
{
    task_header_t *h=&c->pending;
    if (c->failed || c->staging<0 || !h->reserved[1] || device_at<=0) return false;
    c->failed=true; h->device_at=device_at; h->crc=c->running_crc; h->header_crc=header_crc(h);
    /* Validate the entire data readback before publishing the header sector. */
    uint8_t buf[256]; uint32_t crc=~0U,base=c->staging*TASK_SLOT_SIZE;
    for (uint32_t p=0;p<h->bytes;) {
        size_t n=h->bytes-p; if (n>sizeof(buf)) n=sizeof(buf);
        if (!c->io.read(c->io.ctx,base+TASK_DATA_START+p,buf,n)) return false;
        crc=task_crc32(crc,buf,n); p+=n;
    }
    if (crc!=h->crc || !c->io.write(c->io.ctx,base,h,sizeof(*h))) return false;
    /* Reuse the active header as a readback target only after successful write.
       A failed readback keeps the previous active header and slot in RAM. */
    task_header_t check;
    if (!validate(c,c->staging,&check)) return false;
    c->active=check; c->slot=c->staging; c->staging=-1; c->valid=true; return true;
}

bool task_cache_record(task_cache_t *c, unsigned i, task_record_t *out)
{
    if (!c->valid || i>=c->active.count) return false;
    json_t j={.io=&c->io,.base=c->slot*TASK_SLOT_SIZE+c->active.offsets[i],.end=c->active.lengths[i]};
    if (!record(&j,out,c->active.revision,c->active.server_at)) return false;
    ws(&j); return !j.bad && j.pos==j.end;
}

bool task_origin_valid(const char *s)
{
    if (!s || strncmp(s,"https://",8) || strlen(s)>192 || !s[8]) return false;
    size_t n=strlen(s); if (s[n-1]=='/' || s[8]=='.' || s[n-1]=='.') return false;
    size_t label=8;
    for (size_t i=8;i<=n;i++) {
        char c=s[i];
        if (c=='.' || !c) {
            if (i==label || i-label>63 || s[label]=='-' || s[i-1]=='-') return false;
            label=i+1; if (!c) break;
            continue;
        }
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='.' || c=='-')) return false;
    }
    return true;
}
bool task_token_valid(const char *s)
{
    if (!s || strlen(s)<32 || strlen(s)>128) return false;
    for (;*s;s++) if (!((*s>='A'&&*s<='Z') || (*s>='a'&&*s<='z') ||
        (*s>='0'&&*s<='9') || *s=='_' || *s=='-')) return false;
    return true;
}
bool task_cache_stale(const task_cache_t *c, int64_t now, bool trusted)
{
    return !c->valid || c->active.stale || c->active.auth || !trusted || now<c->active.server_at ||
        now-c->active.server_at>=86400;
}
