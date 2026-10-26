#pragma once
#include "suite_app.h"
#include <stdint.h>
#include "suite_task_ops.h"
#include "suite_task_voice.h"

typedef enum {
    TASKS_LOADING, TASKS_UNCONFIGURED, TASKS_READY, TASKS_REFRESHING,
    TASKS_OFFLINE, TASKS_TIME_REQUIRED, TASKS_AUTH_FAILED, TASKS_FAILED,
    TASKS_CANCELLED, TASKS_STORAGE_FAILED, TASKS_SOURCE_AUTH_FAILED, TASKS_UNAVAILABLE
} suite_tasks_state_t;
typedef struct {
    suite_tasks_state_t state;
    bool cached, stale, detail;
    unsigned count, retained_count, selected, note_page, note_pages, serial;
    int64_t server_at, device_at;
    char titles[3][513];
    char status[24], due[320], note[769], collection[65];
    bool truncated, recurring, parent, request_only;
    bool completion_configured,completion_allowed,journal_ready;
    char task_id[65],etag[257],notice[96];
    unsigned pending_count;
    task_op_state_t operation,row_operations[3];
    task_voice_state_t voice_state;
    task_op_state_t voice_operation;
    unsigned voice_seconds;
    char voice_summary[513],voice_collection[257],voice_message[96];
} suite_tasks_view_t;

esp_err_t suite_tasks_start(void);
void suite_tasks_select(unsigned index, bool detail, unsigned note_page);
void suite_tasks_refresh(void);
void suite_tasks_cancel(void);
/* Nonblocking intent submission. Pending is shown only after durable storage. */
bool suite_tasks_complete(const char *task_id,const char *etag);
bool suite_tasks_discard_conflict(const char *task_id);
/* 1 record, 2 stop, 3 confirm create, 4 cancel unconfirmed work, 5 retry,
   6 acknowledge success or discard a proven terminal rejection. Nonblocking. */
bool suite_tasks_voice(unsigned command);
/* Nonblocking snapshot borrow. Caller must release immediately; no I/O. */
const suite_tasks_view_t *suite_tasks_view_acquire(void);
void suite_tasks_view_release(void);
extern const suite_page_t SUITE_PAGE_TASKS;
extern const suite_page_t SUITE_PAGE_TASK_DETAIL;
extern const suite_page_t SUITE_PAGE_TASK_CREATE;
