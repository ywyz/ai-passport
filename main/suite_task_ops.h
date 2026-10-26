#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TASK_OP_CAPACITY 8
typedef enum {
    TASK_OP_NONE=0,TASK_OP_LOCAL,TASK_OP_SERVER,TASK_OP_SUCCEEDED,TASK_OP_CONFLICT,
    TASK_OP_AUTH,TASK_OP_FAILED,TASK_OP_READONLY,TASK_OP_NOT_FOUND,TASK_OP_PAUSED
} task_op_state_t;
typedef struct {
    uint32_t magic,sequence;
    int64_t created_at;
    char operation_id[33],task_id[65],etag[257];
    uint8_t state;
    uint8_t reserved[5];
    uint32_t crc;
} task_operation_t;
typedef struct {
    void *ctx;
    /* Load: 1=present, 0=absent, -1=storage error. */
    int (*load)(void *,unsigned,task_operation_t *);
    bool (*save)(void *,unsigned,const task_operation_t *);
    bool (*erase)(void *,unsigned);
    int (*sequence)(void *,uint32_t *);
    bool (*save_sequence)(void *,uint32_t);
} task_journal_io_t;
typedef struct {
    task_operation_t entries[TASK_OP_CAPACITY];
    task_journal_io_t io;
    uint32_t sequence;
    bool ready;
} task_journal_t;
bool task_id_valid(const char *);
bool task_etag_valid(const char *);
bool task_journal_open(task_journal_t *,task_journal_io_t);
int task_journal_find(const task_journal_t *,const char *task_id);
bool task_journal_add(task_journal_t *,const char *task_id,const char *etag,
                      const char *operation_id,int64_t created_at);
bool task_journal_update(task_journal_t *,unsigned,task_op_state_t);
bool task_journal_discard(task_journal_t *,unsigned);
unsigned task_journal_count(const task_journal_t *);
/* Canonical intent for SHA-256 when digest=NULL; complete POST otherwise. */
bool task_operation_json(const task_operation_t *,char *,size_t,const char *digest,bool retry);
bool task_operation_receipt(const char *,size_t,const task_operation_t *,task_op_state_t *);
/* Bounded admission error; never interpreted as remote success. */
bool task_operation_error(const char *,size_t,task_op_state_t *);
