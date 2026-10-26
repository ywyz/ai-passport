#include "suite_task_ops.h"
#include "suite_tasks_core.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define OP_MAGIC 0x31504f54U
static bool hex(const char *s,size_t n)
{
    if (!s || strlen(s)!=n) return false;
    for (size_t i=0;i<n;i++) if (!(s[i]>='0'&&s[i]<='9') && !(s[i]>='a'&&s[i]<='f')) return false;
    return true;
}
bool task_id_valid(const char *s) { return hex(s,64); }
bool task_etag_valid(const char *s)
{
    if (!s) return false;
    size_t n=strlen(s);
    if (n<3 || n>256 || s[0]!='"' || s[n-1]!='"') return false;
    for (size_t i=1;i+1<n;i++) if ((unsigned char)s[i]<33 || (unsigned char)s[i]>126 || s[i]=='"') return false;
    return true;
}
static uint32_t crc(const task_operation_t *op)
{ return task_crc32(~0U,op,offsetof(task_operation_t,crc)); }
static bool valid(const task_operation_t *op)
{
    return op->magic==OP_MAGIC && op->sequence &&
        memchr(op->operation_id,0,sizeof(op->operation_id)) && memchr(op->task_id,0,sizeof(op->task_id)) &&
        memchr(op->etag,0,sizeof(op->etag)) && hex(op->operation_id,32) && task_id_valid(op->task_id) &&
        task_etag_valid(op->etag) && op->state>=TASK_OP_LOCAL && op->state<=TASK_OP_PAUSED && op->crc==crc(op);
}
bool task_journal_open(task_journal_t *j,task_journal_io_t io)
{
    memset(j,0,sizeof(*j)); j->io=io;
    if (!io.load || !io.save || !io.erase || !io.sequence || !io.save_sequence) return false;
    if (io.sequence(io.ctx,&j->sequence)<0) return false;
    for (unsigned i=0;i<TASK_OP_CAPACITY;i++) {
        int loaded=io.load(io.ctx,i,&j->entries[i]);
        if (loaded<0 || (loaded && !valid(&j->entries[i]))) return false;
        if (!loaded) memset(&j->entries[i],0,sizeof(j->entries[i]));
        if (loaded) for (unsigned k=0;k<i;k++) {
            const task_operation_t *prior=&j->entries[k];
            if (prior->magic && (prior->sequence==j->entries[i].sequence ||
                !strcmp(prior->task_id,j->entries[i].task_id) ||
                !strcmp(prior->operation_id,j->entries[i].operation_id))) return false;
        }
        if (j->entries[i].sequence>j->sequence) j->sequence=j->entries[i].sequence;
    }
    j->ready=true; return true;
}
int task_journal_find(const task_journal_t *j,const char *id)
{
    if (!j->ready || !id) return -1;
    for (unsigned i=0;i<TASK_OP_CAPACITY;i++) if (j->entries[i].magic && !strcmp(j->entries[i].task_id,id)) return (int)i;
    return -1;
}
unsigned task_journal_count(const task_journal_t *j)
{
    if (!j->ready) return 0;
    unsigned n=0;
    for (unsigned i=0;i<TASK_OP_CAPACITY;i++) if (j->entries[i].magic) n++;
    return n;
}
bool task_journal_add(task_journal_t *j,const char *id,const char *etag,const char *opid,int64_t created)
{
    if (!j->ready || !task_id_valid(id) || !task_etag_valid(etag) || !hex(opid,32) ||
        created<0 || j->sequence==UINT32_MAX || task_journal_find(j,id)>=0) return false;
    for (unsigned i=0;i<TASK_OP_CAPACITY;i++)
        if (j->entries[i].magic && !strcmp(j->entries[i].operation_id,opid)) return false;
    unsigned slot=0; while (slot<TASK_OP_CAPACITY && j->entries[slot].magic) slot++;
    if (slot==TASK_OP_CAPACITY) return false;
    task_operation_t candidate={0}; candidate.magic=OP_MAGIC; candidate.sequence=j->sequence+1;
    candidate.created_at=created; candidate.state=TASK_OP_LOCAL;
    strcpy(candidate.task_id,id); strcpy(candidate.etag,etag); strcpy(candidate.operation_id,opid);
    candidate.crc=crc(&candidate);
    if (!j->io.save_sequence(j->io.ctx,candidate.sequence)) return false;
    j->sequence=candidate.sequence;
    if (!j->io.save(j->io.ctx,slot,&candidate)) return false;
    j->entries[slot]=candidate; return true;
}
bool task_journal_update(task_journal_t *j,unsigned slot,task_op_state_t state)
{
    if (!j->ready || slot>=TASK_OP_CAPACITY || !j->entries[slot].magic || state<TASK_OP_LOCAL || state>TASK_OP_PAUSED) return false;
    if (j->entries[slot].state==(unsigned)state) return true;
    task_operation_t candidate=j->entries[slot]; candidate.state=(uint8_t)state; candidate.crc=crc(&candidate);
    if (!j->io.save(j->io.ctx,slot,&candidate)) return false;
    j->entries[slot]=candidate; return true;
}
bool task_journal_discard(task_journal_t *j,unsigned slot)
{
    if (!j->ready || slot>=TASK_OP_CAPACITY || !j->entries[slot].magic || !j->io.erase(j->io.ctx,slot)) return false;
    memset(&j->entries[slot],0,sizeof(j->entries[slot])); return true;
}
bool task_operation_json(const task_operation_t *op,char *out,size_t cap,const char *digest,bool retry)
{
    if (!valid(op) || (digest && !hex(digest,64))) return false;
    char escaped[515],created[24]; size_t used=0;
    for (const char *s=op->etag;*s;s++) {
        if (*s=='"' || *s=='\\') escaped[used++]='\\';
        escaped[used++]=*s;
    }
    escaped[used]=0;
    if (op->created_at) snprintf(created,sizeof(created),"%" PRId64,op->created_at);
    else strcpy(created,"null");
    int n=snprintf(out,cap,"{\"base_etag\":\"%s\",\"created_at\":%s,\"desired\":\"completed\",\"device_sequence\":%" PRIu32 ",\"operation_id\":\"%s\",\"task_id\":\"%s\"}",
                   escaped,created,op->sequence,op->operation_id,op->task_id);
    if (n<0 || (size_t)n>=cap) return false;
    if (digest) {
        int extra=snprintf(out+n-1,cap-(size_t)n+1,",\"payload_hash\":\"%s\",\"retry\":%s}",digest,retry?"true":"false");
        if (extra<0 || (size_t)extra>=cap-(size_t)n+1) return false;
    }
    return true;
}
