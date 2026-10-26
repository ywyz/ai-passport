#include "suite_task_voice.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[2*TASK_SLOT_SIZE];
static bool erase_failed;
static bool read_flash(void *ctx,uint32_t at,void *out,size_t bytes)
{ (void)ctx; assert(at+bytes<=sizeof(memory)); memcpy(out,memory+at,bytes); return true; }
static bool write_flash(void *ctx,uint32_t at,const void *in,size_t bytes)
{ (void)ctx; assert(at+bytes<=sizeof(memory)); const unsigned char *p=in; for (size_t i=0;i<bytes;i++) { assert((memory[at+i]&p[i])==p[i]); memory[at+i]&=p[i]; } return true; }
static bool erase_flash(void *ctx,uint32_t at,size_t bytes)
{ (void)ctx; assert(!(at%4096) && !(bytes%4096)); if (erase_failed) return false; memset(memory+at,255,bytes); return true; }
int main(void)
{
    task_cache_t cache={.io={.read=read_flash,.write=write_flash,.erase=erase_flash},.valid=true,.slot=1};
    memset(memory,0x77,sizeof(memory)); uint32_t base;
    assert(task_voice_stage_begin(&cache,&base) && base==TASK_DATA_START);
    unsigned char wav[44]; task_voice_wav(wav,32000);
    assert(!memcmp(wav,"RIFF",4) && !memcmp(wav+8,"WAVEfmt ",8) && wav[22]==1 && wav[34]==16);
    assert(write_flash(NULL,base,wav,44));
    for (unsigned i=TASK_SLOT_SIZE;i<sizeof(memory);i++) assert(memory[i]==0x77);
    erase_failed=true; assert(!task_voice_stage_clear(&cache));
    erase_failed=false; assert(task_voice_stage_clear(&cache));
    for (unsigned i=0;i<TASK_VOICE_ERASE;i++) assert(memory[i]==255);
    for (unsigned i=TASK_SLOT_SIZE;i<sizeof(memory);i++) assert(memory[i]==0x77);
    assert(task_voice_title("买牛奶")); assert(!task_voice_title("")); assert(!task_voice_title(" x"));
    assert(!task_voice_title("x\n")); assert(!task_voice_title("\xed\xa0\x80")); assert(!task_voice_title("\xc0\xaf"));
    task_creation_t intent={0}; memset(intent.operation_id,'f',32); memset(intent.collection_id,'a',64);
    strcpy(intent.summary,"买牛奶"); intent.state=TASK_OP_LOCAL; task_creation_seal(&intent);
    assert(task_creation_valid(&intent));
    char json[1400]; assert(task_creation_json(&intent,json,sizeof(json),NULL,false));
    assert(!strcmp(json,"{\"collection_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"operation_id\":\"ffffffffffffffffffffffffffffffff\",\"summary\":\"买牛奶\"}"));
    assert(!task_creation_json(&intent,json,10,NULL,false));
    char digest[65]; memset(digest,'b',64); digest[64]=0;
    snprintf(json,sizeof(json),"{\"schema\":1,\"operation_id\":\"%s\",\"collection_id\":\"%s\",\"payload_hash\":\"%s\",\"state\":\"SUCCEEDED\"}",intent.operation_id,intent.collection_id,digest);
    task_op_state_t state;
    assert(task_creation_receipt(json,strlen(json),&intent,digest,&state) && state==TASK_OP_SUCCEEDED);
    digest[0]='c'; assert(!task_creation_receipt(json,strlen(json),&intent,digest,&state));
    intent.summary[0]='x'; assert(!task_creation_valid(&intent));
    char collection[65],name[257],summary[513];
    snprintf(json,sizeof(json),"{\"schema\":1,\"summary\":\"买牛奶\",\"collection_id\":\"%s\",\"collection_name\":\"任务\"}",intent.collection_id);
    assert(task_voice_response(json,strlen(json),true,collection,name,summary));
    assert(!strcmp(summary,"买牛奶") && !strcmp(name,"任务"));
    assert(!task_voice_response(json,strlen(json),false,collection,name,summary));
    assert(task_voice_error_classify(401,"",0)==TASK_VOICE_ERROR_AUTH);
    assert(task_voice_error_classify(403,"{}",2)==TASK_VOICE_ERROR_AUTH);
    const char *error;
    error="{\"error\":\"AUTH_FAILED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_AUTH);
    error="{\"error\":\"SPEECH_AUTH_FAILED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_AUTH);
    error="{\"error\":\"SPEECH_BUDGET_EXCEEDED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_BUDGET);
    error="{\"error\":\"SPEECH_UNAVAILABLE\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_RETRY);
    error="{\"error\":\"SPEECH_TIMEOUT\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_RETRY);
    error="{\"error\":\"CREATION_TARGET_REQUIRED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_TARGET_REQUIRED);
    error="{\"error\":\"OTHER\",\"error\":\"CREATION_TARGET_REQUIRED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="{\"error\":\"CREATION_TARGET_REQUIRED\"}x";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="{\"detail\":\"CREATION_TARGET_REQUIRED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="{\"error\":7}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="private upstream message";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="{\"error\":\"CREATION_TARGET_REQUIRED\\u0000extra\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    error="{\"error\\u0000extra\":\"CREATION_TARGET_REQUIRED\"}";
    assert(task_voice_error_classify(400,error,strlen(error))==TASK_VOICE_ERROR_UNKNOWN);
    char raw_value[]="{\"error\":\"CREATION_TARGET_REQUIRED\0extra\"}";
    assert(task_voice_error_classify(400,raw_value,sizeof(raw_value)-1)==TASK_VOICE_ERROR_UNKNOWN);
    char raw_key[]="{\"error\0extra\":\"CREATION_TARGET_REQUIRED\"}";
    assert(task_voice_error_classify(400,raw_key,sizeof(raw_key)-1)==TASK_VOICE_ERROR_UNKNOWN);
    puts("voice Flash isolation / cleanup retry / PCM envelope / creation journal / JSON binding / strict error classification: PASS");
}
