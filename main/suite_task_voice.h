#pragma once
#include "suite_tasks_core.h"
#include "suite_task_ops.h"

#define TASK_VOICE_PCM_MAX (12U*16000U*2U)
#define TASK_VOICE_ERASE (((TASK_DATA_START+TASK_VOICE_PCM_MAX+44U+4095U)/4096U)*4096U)
typedef enum { VOICE_READY,VOICE_CHECKING,VOICE_RECORDING,VOICE_RECOGNIZING,
    VOICE_PREVIEW,VOICE_PENDING,VOICE_SUCCEEDED,VOICE_FAILED,VOICE_STORAGE_FAILED } task_voice_state_t;
/* Strict, body-only classification for voice admission/ASR errors. */
typedef enum { TASK_VOICE_ERROR_UNKNOWN=0,TASK_VOICE_ERROR_AUTH,TASK_VOICE_ERROR_BUDGET,
    TASK_VOICE_ERROR_RETRY,TASK_VOICE_ERROR_TARGET_REQUIRED } task_voice_error_t;
typedef struct {
    uint32_t magic;
    char operation_id[33],collection_id[65],summary[513];
    uint8_t state,reserved[4];
    uint32_t crc;
} task_creation_t;
/* One Flash owner, outside the active cache generation. No audio RAM buffer. */
bool task_voice_stage_begin(task_cache_t *,uint32_t *base);
bool task_voice_stage_clear(task_cache_t *);
/* Slot-addressed primitives let a shared outer owner choose the inactive slot
   explicitly. The legacy wrappers above keep their historical two-slot scan. */
bool task_voice_stage_begin_span(task_cache_t *,unsigned slot,uint32_t span,uint32_t *base);
bool task_voice_stage_clear_slot(task_cache_t *,unsigned slot);
/* 1=marked, 0=confirmed unmarked, -1=marker read failure. Callers must not
   treat a read failure as clean and must not clear quarantine for it. */
int task_voice_stage_marker(task_cache_t *,unsigned slot);
bool task_voice_stage_marked(task_cache_t *,unsigned slot);
void task_voice_wav(unsigned char header[44],uint32_t pcm_bytes);
bool task_voice_title(const char *);
bool task_creation_valid(const task_creation_t *);
void task_creation_seal(task_creation_t *);
bool task_creation_json(const task_creation_t *,char *,size_t,const char *digest,bool retry);
bool task_creation_receipt(const char *,size_t,const task_creation_t *,const char *digest,task_op_state_t *);
bool task_voice_response(const char *,size_t,bool transcript,char *collection,char *name,char *summary);
task_voice_error_t task_voice_error_classify(int status,const char *,size_t);
