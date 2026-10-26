#include "suite_task_ops.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static task_operation_t storage[TASK_OP_CAPACITY];
static uint32_t sequence;
static bool fail_save,fail_sequence;
static unsigned writes,erases;
static int load(void *p,unsigned slot,task_operation_t *op)
{ (void)p; *op=storage[slot]; return op->magic ? 1 : 0; }
static bool save(void *p,unsigned slot,const task_operation_t *op)
{ (void)p; writes++; if (fail_save) return false; storage[slot]=*op; return true; }
static bool erase(void *p,unsigned slot)
{ (void)p; erases++; memset(&storage[slot],0,sizeof(storage[slot])); return true; }
static int seq(void *p,uint32_t *out) { (void)p; *out=sequence; return sequence ? 1 : 0; }
static bool setseq(void *p,uint32_t value) { (void)p; if (fail_sequence) return false; sequence=value; return true; }
static task_journal_io_t io={.load=load,.save=save,.erase=erase,.sequence=seq,.save_sequence=setseq};

int main(void)
{
    task_journal_t j,reopened; char id[65],opid[33],json[1400],receipt[512];
    memset(id,'a',64); id[64]=0; memset(opid,'1',32); opid[32]=0;
    assert(task_journal_open(&j,io));
    fail_sequence=true; assert(!task_journal_add(&j,id,"\"base\"",opid,0));
    assert(!task_journal_count(&j) && !writes); fail_sequence=false;
    fail_save=true; assert(!task_journal_add(&j,id,"\"base\"",opid,0));
    assert(!task_journal_count(&j)); fail_save=false;
    assert(task_journal_add(&j,id,"\"base\"",opid,0));
    assert(task_journal_count(&j)==1 && task_journal_find(&j,id)==0);
    assert(!task_journal_add(&j,id,"\"base\"",opid,0));
    assert(task_operation_json(&j.entries[0],json,sizeof(json),NULL,false));
    char expected[1000];
    snprintf(expected,sizeof(expected),"{\"base_etag\":\"\\\"base\\\"\",\"created_at\":null,\"desired\":\"completed\",\"device_sequence\":2,\"operation_id\":\"%s\",\"task_id\":\"%s\"}",opid,id);
    assert(!strcmp(json,expected));
    assert(!task_operation_json(&j.entries[0],json,16,NULL,false));
    assert(task_operation_json(&j.entries[0],json,sizeof(json),id,true));
    assert(strstr(json,"\"retry\":true") && strstr(json,"\"payload_hash\""));
    assert(task_journal_open(&reopened,io));
    assert(!strcmp(reopened.entries[0].operation_id,opid));
    unsigned before=writes;
    assert(task_journal_update(&j,0,TASK_OP_LOCAL) && writes==before);
    fail_save=true; assert(!task_journal_update(&j,0,TASK_OP_SUCCEEDED));
    assert(j.entries[0].state==TASK_OP_LOCAL); fail_save=false;
    assert(task_journal_update(&j,0,TASK_OP_SERVER));
    assert(task_journal_open(&reopened,io) && reopened.entries[0].state==TASK_OP_SERVER);
    for (unsigned i=1;i<TASK_OP_CAPACITY;i++) {
        id[0]=(char)('a'+i%6); id[1]=(char)('0'+i);
        assert(!task_journal_add(&j,id,"\"base\"",opid,1792500000));
        opid[0]=(char)('1'+i);
        assert(task_journal_add(&j,id,"\"base\"",opid,1792500000));
    }
    id[0]='f'; id[1]='f'; before=writes;
    assert(!task_journal_add(&j,id,"\"base\"",opid,0) && writes==before);
    task_operation_t *op=&j.entries[0]; task_op_state_t state;
    snprintf(receipt,sizeof(receipt),"{\"schema\":1,\"operation_id\":\"%s\",\"task_id\":\"%s\",\"state\":\"SUCCEEDED\",\"error\":null,\"updated_at\":1792500000}",op->operation_id,op->task_id);
    assert(task_operation_receipt(receipt,strlen(receipt),op,&state) && state==TASK_OP_SUCCEEDED);
    receipt[31]='z'; assert(!task_operation_receipt(receipt,strlen(receipt),op,&state));
    const char *error="{\"error\":\"READ_ONLY\"}";
    assert(task_operation_error(error,strlen(error),&state) && state==TASK_OP_READONLY);
    error="{\"error\":\"SNAPSHOT_NOT_READY\"}";
    assert(task_operation_error(error,strlen(error),&state) && state==TASK_OP_PAUSED);
    error="{\"error\":\"SUCCEEDED\",\"error\":\"CONFLICT\"}";
    assert(!task_operation_error(error,strlen(error),&state));
    task_operation_t original=storage[1]; storage[1]=storage[0];
    assert(!task_journal_open(&reopened,io) && !task_journal_count(&reopened));
    storage[1]=original;
    storage[0].crc^=1; assert(!task_journal_open(&reopened,io)); assert(!erases);
    assert(task_journal_discard(&j,1) && erases==1);
    assert(!task_etag_valid("*") && !task_etag_valid("W/\"base\"") && !task_etag_valid("\"line\r\n\""));
    puts("completion journal / commit failures / restart / capacity / canonical payload / receipt identity: PASS");
    return 0;
}
