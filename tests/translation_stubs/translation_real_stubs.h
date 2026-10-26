#pragma once
#include "suite_translation_config.h"
#include "suite_translation_http.h"
#include "suite_translation_protocol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    TR_ORDER_READY = 1,
    TR_ORDER_ACQUIRE,
    TR_ORDER_STAGE,
    TR_ORDER_AUDIO_INIT,
    TR_ORDER_AUDIO_RELEASE,
    TR_ORDER_ADMIT,
    TR_ORDER_UPLOAD,
    TR_ORDER_CLEANUP,
    TR_ORDER_STATUS,
    TR_ORDER_CANCEL,
};

typedef struct {
    suite_translation_config_status_t config_status;
    bool online;
    bool time_trusted;
    bool usb_window_open;
    bool usb_open_result;
    bool audio_init_fail;
    bool audio_format_fail;
    bool audio_read_fail;
    unsigned audio_read_fail_at;
    bool audio_release_fail;
    bool acquire_fail;
    bool stage_fail;
    bool append_fail;
    bool finalize_fail;
    bool sha_fail;
    bool cleanup_fail;
    unsigned pcm_max;
    unsigned pcm_bytes;
    unsigned read_delay_us;
    unsigned audio_reads;
    unsigned acquire_calls;
    unsigned stage_calls;
    unsigned cleanup_calls;
    unsigned audio_release_calls;
    unsigned http_ready_calls;
    unsigned http_admit_calls;
    unsigned http_upload_calls;
    unsigned http_status_calls;
    unsigned http_cancel_calls;
    suite_translation_http_result_t ready_result;
    suite_translation_http_result_t admit_result;
    suite_translation_http_result_t upload_result;
    suite_translation_http_result_t status_result;
    suite_translation_http_result_t cancel_result;
    bool ready_payload;
    unsigned ready_http_status;
    bool ready_tls_diag_valid;
    suite_translation_tls_diag_t ready_tls_diag;
    bool exit_during_ready;
    bool admit_ambiguous;
    bool upload_ambiguous;
    bool status_ambiguous;
    bool admit_bad_identity;
    bool cancel_during_ready;
    bool cancel_during_admit;
    bool cancel_during_upload;
    bool cancel_during_status;
    char admitted_sha[65];
    unsigned upload_reader_bytes;
    unsigned status_count;
    unsigned status_index;
    unsigned heap_free_bytes;
    unsigned heap_largest_bytes;
    unsigned heap_free_calls;
    unsigned heap_largest_calls;
    unsigned heap_free_first;
    unsigned heap_largest_first;
    bool cancel_on_sha;
    bool exit_on_sha;
    bool cancel_on_heap_sample;
    bool exit_on_heap_sample;
    bool heap_hook_fired;
    suite_translation_protocol_state_t status_states[8];
    bool status_source;
    bool status_target;
    int order[32];
    unsigned order_count;
} translation_real_stub_t;

extern translation_real_stub_t tr_stub;
void translation_real_stub_reset(void);
