#include "suite_tasks_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char flash[2*TASK_SLOT_SIZE];
static size_t cut=SIZE_MAX;
static bool read_flash(void *p,uint32_t at,void *out,size_t n)
{ (void)p; if (at+n>sizeof(flash)) return false; memcpy(out,flash+at,n); return true; }
static bool write_flash(void *p,uint32_t at,const void *data,size_t n)
{
    (void)p; if (at+n>sizeof(flash)) return false;
    size_t m=n<cut ? n : cut;
    const unsigned char *bytes=data;
    for (size_t i=0;i<m;i++) { assert((flash[at+i]&bytes[i])==bytes[i]); flash[at+i]&=bytes[i]; }
    cut-=m; return m==n;
}
static bool erase_flash(void *p,uint32_t at,size_t n)
{ (void)p; if (at%4096 || n%4096 || at+n>sizeof(flash)) return false; memset(flash+at,255,n); return true; }
static task_io_t io={.read=read_flash,.write=write_flash,.erase=erase_flash};
static const char *rev="0123456789abcdef0123456789abcdef";
static char item[9000],page[32770];
static task_record_t decoded;

static void record_fixture(const char *summary,const char *note)
{
    snprintf(item,sizeof(item),"{\"schema\":1,\"account_id\":\"account\",\"collection_id\":\"collection\","
        "\"uid\":\"todo-1\",\"etag\":\"etag-1\",\"href\":\"https://example.test/a.ics\",\"recurrence_id\":null,"
        "\"summary\":\"%s\",\"note\":\"%s\",\"status\":\"NEEDS-ACTION\",\"percent_complete\":0,"
        "\"note_truncated\":false,\"recurring\":false,\"has_parent\":false,\"read_only\":true,"
        "\"due\":{\"kind\":\"date\",\"value\":\"20261019\",\"display\":\"2026-10-19\"},"
        "\"snapshot_revision\":\"%s\",\"downloaded_at\":1792368000}",summary,note,rev);
}
static void page_fixture(unsigned offset,unsigned total,const char *next,const char *items)
{
    snprintf(page,sizeof(page),"{\"items\":[%s],\"schema\":1,\"snapshot_revision\":\"%s\","
        "\"selection_revision\":2,\"downloaded_at\":1792368000,\"read_only\":true,"
        "\"state\":\"READY\",\"offset\":%u,\"total\":%u,\"next_offset\":%s}",items,rev,offset,total,next);
}
static bool submit(task_cache_t *c)
{
    uint32_t at=c->staging*TASK_SLOT_SIZE+TASK_SCRATCH;
    assert(erase_flash(NULL,at,TASK_PAGE_MAX));
    assert(write_flash(NULL,at,page,strlen(page)));
    task_page_t meta;
    return task_cache_page(c,strlen(page),&meta,&decoded);
}
static void publish(task_cache_t *c,const char *title)
{
    assert(task_cache_begin(c)); record_fixture(title,"Note");
    page_fixture(0,1,"null",item); assert(submit(c)); assert(task_cache_commit(c,1792368010));
}

int main(int argc,char **argv)
{
    task_cache_t c,reopened;
    memset(flash,255,sizeof(flash));
    assert(!task_cache_open(&c,io));
    if (argc>1) {
        /* Optional private acceptance replay. Never print task contents. */
        assert(task_cache_begin(&c));
        for (int i=1;i<argc;i++) {
            FILE *f=fopen(argv[i],"rb"); assert(f);
            size_t n=fread(page,1,sizeof(page)-1,f); assert(!ferror(f) && feof(f)); fclose(f);
            assert(n<=TASK_PAGE_MAX); page[n]=0; assert(submit(&c));
        }
        assert(task_cache_commit(&c,c.pending.server_at+1));
        assert(task_cache_open(&reopened,io));
        for (unsigned i=0;i<reopened.active.count;i++) assert(task_cache_record(&reopened,i,&decoded));
        printf("private snapshot host replay: PASS, %u records, %u pages\n",reopened.active.count,argc-1);
        return 0;
    }
    publish(&c,"First");
    assert(task_cache_record(&c,0,&decoded)); assert(!strcmp(decoded.summary,"First"));
    assert(task_cache_open(&reopened,io)); assert(reopened.active.count==1);
    assert(!task_cache_stale(&c,1792368011,true));
    assert(task_cache_stale(&c,1792368000+86400,true));
    assert(task_cache_stale(&c,1792368011,false));
    assert(task_cache_stale(&c,1792367999,true));

    /* Interrupted replacement cannot delete the old committed snapshot. */
    assert(task_cache_begin(&c)); record_fixture("New","Notes");
    page_fixture(0,2,"1",item); assert(submit(&c));
    assert(!task_cache_commit(&c,1792368011));
    assert(task_cache_open(&reopened,io)); assert(reopened.active.count==1);
    page_fixture(1,2,"null",item); assert(submit(&c));
    assert(task_cache_commit(&c,1792368020));
    assert(task_cache_open(&reopened,io)); assert(reopened.active.count==2);

    /* Newest corruption falls back to the complete previous slot. */
    flash[c.slot*TASK_SLOT_SIZE+TASK_DATA_START]^=1;
    assert(task_cache_open(&reopened,io)); assert(reopened.active.count==1);
    c=reopened;
    const size_t cuts[]={0,1,4,23,31,128,800,offsetof(task_header_t,header_crc)};
    for (unsigned i=0;i<sizeof(cuts)/sizeof(cuts[0]);i++) {
        assert(task_cache_begin(&c)); record_fixture("Torn","N"); page_fixture(0,1,"null",item); assert(submit(&c));
        cut=cuts[i]; assert(!task_cache_commit(&c,1792368020)); cut=SIZE_MAX;
        assert(task_cache_open(&reopened,io)); assert(reopened.active.count==1);
        assert(task_cache_record(&reopened,0,&decoded)); assert(!strcmp(decoded.summary,"First"));
        c=reopened;
    }

    /* Cursor, revision and item revision must agree on every page. */
    assert(task_cache_begin(&c)); record_fixture("A","N"); page_fixture(0,2,"1",item); assert(submit(&c));
    page_fixture(1,2,"null",item); char *q=strstr(page,"0123456789abcdef"); assert(q); *q='f';
    assert(!submit(&c)); assert(!task_cache_commit(&c,1792368020));
    assert(task_cache_begin(&c)); record_fixture("A","N"); page_fixture(1,1,"null",item); assert(!submit(&c));
    assert(task_cache_begin(&c)); page_fixture(0,2,"0",item); assert(!submit(&c));
    assert(task_cache_begin(&c)); page_fixture(0,201,"null",item); assert(!submit(&c));
    assert(task_cache_begin(&c)); page_fixture(0,2,"null",item); assert(!submit(&c));

    /* Exact limits, UTF-8, escaped surrogate pairs, invalid/duplicate fields. */
    record_fixture("\\u4e2d\\u6587 \\ud83d\\ude00","line1\\nline2");
    assert(task_record_parse(item,strlen(item),&decoded));
    assert(!strcmp(decoded.summary,"\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80"));
    record_fixture("\\ud800","N"); assert(!task_record_parse(item,strlen(item),&decoded));
    record_fixture("\\u0000suffix","N"); assert(!task_record_parse(item,strlen(item),&decoded));
    record_fixture("\xc0\xaf","N"); assert(!task_record_parse(item,strlen(item),&decoded));
    char long_title[514]; memset(long_title,'a',512); long_title[512]=0;
    record_fixture(long_title,"N"); assert(task_record_parse(item,strlen(item),&decoded));
    long_title[512]='a'; long_title[513]=0;
    record_fixture(long_title,"N"); assert(!task_record_parse(item,strlen(item),&decoded));
    char note[4098]; memset(note,'n',4096); note[4096]=0;
    record_fixture("A",note); assert(task_record_parse(item,strlen(item),&decoded));
    note[4096]='n'; note[4097]=0; record_fixture("A",note); assert(!task_record_parse(item,strlen(item),&decoded));
    record_fixture("A","N"); size_t length=strlen(item); item[length-1]=',';
    strcpy(item+length,"\"schema\":1}"); assert(!task_record_parse(item,strlen(item),&decoded));

    /* Valid empty snapshots replace older contents, but never partial ones. */
    assert(task_cache_begin(&c)); page_fixture(0,0,"null",""); assert(submit(&c));
    assert(task_cache_commit(&c,1792368030)); assert(c.active.count==0);
    assert(task_cache_open(&reopened,io)); assert(reopened.active.count==0);
    /* An interrupted data write cannot create a committed header. */
    c=reopened; assert(task_cache_begin(&c)); record_fixture("Data cut","N");
    page_fixture(0,1,"null",item); cut=strlen(page)+32;
    assert(!submit(&c)); cut=SIZE_MAX;
    assert(task_cache_open(&reopened,io) && reopened.active.count==0);
    /* Aggregate Flash budget is enforced even with valid individual records. */
    c=reopened; assert(task_cache_begin(&c));
    char extra[1001]; memset(extra,'x',1000); extra[1000]=0;
    memset(long_title,'a',512); long_title[512]=0;
    memset(note,'n',4096); note[4096]=0;
    record_fixture(long_title,note);
    size_t raw=strlen(item); item[raw-1]=0;
    snprintf(item+raw-1,sizeof(item)-raw+1,",\"extra\":\"%s\"}",extra);
    assert(strlen(item)<TASK_RECORD_MAX);
    bool exceeded=false;
    for (unsigned i=0;i<TASK_MAX_ITEMS;i++) {
        char next[12]; snprintf(next,sizeof(next),"%u",i+1);
        page_fixture(i,TASK_MAX_ITEMS,i+1==TASK_MAX_ITEMS ? "null" : next,item);
        if (!submit(&c)) { exceeded=true; break; }
    }
    assert(exceeded && c.pending.bytes<=TASK_CACHE_MAX);
    assert(!task_cache_commit(&c,1792368031));
    assert(task_cache_open(&reopened,io) && reopened.active.count==0);
    assert(task_origin_valid("https://tasks.example.test"));
    const char *bad[]={"http://example.test","https://user@example.test","https://example.test/",
        "https://example.test:443","https://example.test?q=x","https://-example.test","https://a..b"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!task_origin_valid(bad[i]));
    assert(task_token_valid("abcdefghijklmnopqrstuvwxyz_0123456789"));
    assert(!task_token_valid("short")); assert(!task_token_valid("abcdefghijklmnopqrstuvwxyz\r\nHeader: secret"));
    puts("task cache / parser / interruption / token policy: PASS"); return 0;
}
