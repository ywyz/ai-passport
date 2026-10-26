#include "suite_task_voice.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define AUDIO_MAGIC 0x31445541U
#define CREATE_MAGIC 0x31524354U

int task_voice_stage_marker(task_cache_t *c,unsigned slot)
{
    if (!c || slot>1) return -1;
    uint32_t magic=0;
    if (!c->io.read(c->io.ctx,slot*TASK_SLOT_SIZE,&magic,sizeof(magic))) return -1;
    return magic==AUDIO_MAGIC ? 1 : 0;
}
bool task_voice_stage_marked(task_cache_t *c,unsigned slot)
{
    return task_voice_stage_marker(c,slot)==1;
}
bool task_voice_stage_clear_slot(task_cache_t *c,unsigned slot)
{
    if (!c || slot>1) return false;
    int marker=task_voice_stage_marker(c,slot);
    if (marker<0) return false; /* unreadable marker: never report clean */
    if (!marker) return true;
    uint32_t base=slot*TASK_SLOT_SIZE;
    /* Data goes before the marker sector, so a failed cleanup never leaves a
       marker that could make stale audio look current. */
    if (!c->io.erase(c->io.ctx,base+4096,TASK_VOICE_ERASE-4096) ||
        !c->io.erase(c->io.ctx,base,4096)) return false;
    return true;
}
bool task_voice_stage_clear(task_cache_t *c)
{
    for (unsigned slot=0;slot<2;slot++)
        if (!task_voice_stage_clear_slot(c,slot)) return false;
    return true;
}
bool task_voice_stage_begin_span(task_cache_t *c,unsigned slot,uint32_t span,uint32_t *base)
{
    if (!c || !base || slot>1 || span<TASK_DATA_START+4096U ||
        span>TASK_SLOT_SIZE || span%4096U) return false;
    uint32_t start=slot*TASK_SLOT_SIZE,magic=AUDIO_MAGIC;
    /* Invalidate the inactive generation before overwriting any audio sectors. */
    if (!c->io.erase(c->io.ctx,start,4096) || !c->io.write(c->io.ctx,start,&magic,sizeof(magic)) ||
        !c->io.erase(c->io.ctx,start+4096,span-4096)) return false;
    *base=start+TASK_DATA_START; return true;
}
bool task_voice_stage_begin(task_cache_t *c,uint32_t *base)
{
    unsigned slot=c->valid ? (unsigned)(1-c->slot) : 0;
    return task_voice_stage_begin_span(c,slot,TASK_VOICE_ERASE,base);
}
static void le(unsigned char *p,uint32_t value,unsigned bytes)
{ for (unsigned i=0;i<bytes;i++) p[i]=(unsigned char)(value>>(8*i)); }
void task_voice_wav(unsigned char h[44],uint32_t bytes)
{
    memset(h,0,44); memcpy(h,"RIFF",4); le(h+4,bytes+36,4); memcpy(h+8,"WAVEfmt ",8);
    le(h+16,16,4); le(h+20,1,2); le(h+22,1,2); le(h+24,16000,4); le(h+28,32000,4);
    le(h+32,2,2); le(h+34,16,2); memcpy(h+36,"data",4); le(h+40,bytes,4);
}
bool task_voice_title(const char *s)
{
    if (!s) return false;
    size_t n=strlen(s);
    if (!n || n>512 || (unsigned char)s[0]<=32 || (unsigned char)s[n-1]<=32) return false;
    for (size_t i=0;i<n;) {
        unsigned char c=(unsigned char)s[i++]; unsigned more; uint32_t value;
        if (c<0x80) { if (c<32 || c==127) return false; continue; }
        if (c>=0xc2 && c<=0xdf) { more=1; value=c&31; }
        else if (c>=0xe0 && c<=0xef) { more=2; value=c&15; }
        else if (c>=0xf0 && c<=0xf4) { more=3; value=c&7; }
        else return false;
        unsigned width=more;
        while (more--) { if (i>=n || ((unsigned char)s[i]&0xc0)!=0x80) return false; value=(value<<6)|((unsigned char)s[i++]&63); }
        if ((width==1 && value<0x80) || (width==2 && value<0x800) || (width==3 && value<0x10000) ||
            (value>=0xd800 && value<=0xdfff) || value>0x10ffff || (value>=127 && value<160)) return false;
    }
    return true;
}
static bool opid(const char *id)
{ if (strlen(id)!=32) return false; for (unsigned i=0;i<32;i++) if (!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f'))) return false; return true; }
void task_creation_seal(task_creation_t *c)
{ c->magic=CREATE_MAGIC; c->crc=task_crc32(~0U,c,offsetof(task_creation_t,crc)); }
bool task_creation_valid(const task_creation_t *c)
{
    return c->magic==CREATE_MAGIC && memchr(c->operation_id,0,sizeof(c->operation_id)) &&
        memchr(c->collection_id,0,sizeof(c->collection_id)) && memchr(c->summary,0,sizeof(c->summary)) &&
        opid(c->operation_id) && task_id_valid(c->collection_id) && task_voice_title(c->summary) &&
        c->state>=TASK_OP_LOCAL && c->state<=TASK_OP_PAUSED && c->crc==task_crc32(~0U,c,offsetof(task_creation_t,crc));
}
bool task_creation_json(const task_creation_t *c,char *out,size_t size,const char *digest,bool retry)
{
    if (!task_creation_valid(c)) return false;
    cJSON *j=cJSON_CreateObject(); if (!j) return false;
    bool ok=cJSON_AddStringToObject(j,"collection_id",c->collection_id) && cJSON_AddStringToObject(j,"operation_id",c->operation_id);
    if (digest) ok=ok && task_id_valid(digest) && cJSON_AddStringToObject(j,"payload_hash",digest) && cJSON_AddBoolToObject(j,"retry",retry);
    ok=ok && cJSON_AddStringToObject(j,"summary",c->summary);
    if (ok) ok=cJSON_PrintPreallocated(j,out,(int)size,false);
    cJSON_Delete(j); return ok;
}
static cJSON *object(const char *raw,size_t bytes)
{
    if (!raw || !bytes || bytes>2048 || memchr(raw,0,bytes)) return NULL;
    for (size_t i=0;i+6<=bytes;i++) if (!memcmp(raw+i,"\\u0000",6)) return NULL;
    const char *end=NULL; cJSON *j=cJSON_ParseWithLengthOpts(raw,bytes,&end,false);
    if (!j) return NULL;
    while (end<raw+bytes && (*end==' '||*end=='\r'||*end=='\n'||*end=='\t')) end++;
    if (end!=raw+bytes || !cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    for (cJSON *a=j->child;a;a=a->next) for (cJSON *b=a->next;b;b=b->next)
        if (!strcmp(a->string,b->string)) { cJSON_Delete(j); return NULL; }
    cJSON *schema=cJSON_GetObjectItemCaseSensitive(j,"schema");
    if (!cJSON_IsNumber(schema) || schema->valuedouble!=1) { cJSON_Delete(j); return NULL; }
    return j;
}
static const char *string(cJSON *j,const char *key)
{ cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v) ? v->valuestring : ""; }
bool task_creation_receipt(const char *raw,size_t bytes,const task_creation_t *c,const char *digest,task_op_state_t *state)
{
    cJSON *j=object(raw,bytes); if (!j) return false;
    const char *s=string(j,"state"); bool ok=!strcmp(string(j,"operation_id"),c->operation_id) &&
        !strcmp(string(j,"collection_id"),c->collection_id) && !strcmp(string(j,"payload_hash"),digest);
    const char *names[]={"SERVER_DURABLE","REMOTE_APPLYING","RETRY_WAIT","SUCCEEDED","CONFLICT","READ_ONLY","NOT_FOUND","AUTH_FAILED","RETRY_PAUSED"};
    task_op_state_t states[]={TASK_OP_SERVER,TASK_OP_SERVER,TASK_OP_SERVER,TASK_OP_SUCCEEDED,TASK_OP_CONFLICT,TASK_OP_READONLY,TASK_OP_NOT_FOUND,TASK_OP_AUTH,TASK_OP_PAUSED};
    bool found=false;
    for (unsigned i=0;i<sizeof(states)/sizeof(states[0]);i++) if (!strcmp(s,names[i])) { *state=states[i]; found=true; break; }
    cJSON_Delete(j); return ok && found;
}
bool task_voice_response(const char *raw,size_t bytes,bool transcript,char *collection,char *name,char *summary)
{
    cJSON *j=object(raw,bytes); if (!j) return false;
    const char *id=string(j,"collection_id"),*label=string(j,"collection_name"),*text=string(j,"summary");
    bool ok=task_id_valid(id) && strlen(label)<=256;
    if (transcript) ok=ok && task_voice_title(text);
    else ok=ok && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"ready"));
    if (ok) { strcpy(collection,id); strcpy(name,label); if (transcript) strcpy(summary,text); }
    cJSON_Delete(j); return ok;
}
/* Only one exact {"error":"..."} object is accepted. Duplicate keys, trailing
   bytes, missing/non-string values and private upstream text stay UNKNOWN.
   This is deliberately separate from completion-queue error semantics. */
task_voice_error_t task_voice_error_classify(int status,const char *raw,size_t bytes)
{
    if (status==401 || status==403) return TASK_VOICE_ERROR_AUTH;
    if (!raw || !bytes || bytes>1024) return TASK_VOICE_ERROR_UNKNOWN;
    /* cJSON decodes \u0000 to an embedded NUL and strcmp would then truncate;
       reject raw NUL and the escape in keys or values before parsing. */
    if (memchr(raw,0,bytes)) return TASK_VOICE_ERROR_UNKNOWN;
    for (size_t i=0;i+6<=bytes;i++)
        if (!memcmp(raw+i,"\\u0000",6)) return TASK_VOICE_ERROR_UNKNOWN;
    const char *end=NULL;
    cJSON *j=cJSON_ParseWithLengthOpts(raw,bytes,&end,false);
    if (!j) return TASK_VOICE_ERROR_UNKNOWN;
    while (end<raw+bytes && (*end==' '||*end=='\r'||*end=='\n'||*end=='\t')) end++;
    if (end!=raw+bytes || !cJSON_IsObject(j)) { cJSON_Delete(j); return TASK_VOICE_ERROR_UNKNOWN; }
    unsigned count=0; const char *code=NULL; bool valid=true;
    for (cJSON *item=j->child;item;item=item->next) {
        count++;
        if (count!=1 || !item->string || strcmp(item->string,"error") || !cJSON_IsString(item)) {
            valid=false; break;
        }
        code=item->valuestring;
    }
    task_voice_error_t result=TASK_VOICE_ERROR_UNKNOWN;
    if (valid && count==1 && code) {
        if (!strcmp(code,"CREATION_TARGET_REQUIRED")) result=TASK_VOICE_ERROR_TARGET_REQUIRED;
        else if (!strcmp(code,"SPEECH_AUTH_FAILED") || !strcmp(code,"AUTH_FAILED")) result=TASK_VOICE_ERROR_AUTH;
        else if (!strcmp(code,"SPEECH_BUDGET_EXCEEDED")) result=TASK_VOICE_ERROR_BUDGET;
        else if (!strcmp(code,"SPEECH_UNAVAILABLE") || !strcmp(code,"SPEECH_TIMEOUT")) result=TASK_VOICE_ERROR_RETRY;
    }
    cJSON_Delete(j); return result;
}
