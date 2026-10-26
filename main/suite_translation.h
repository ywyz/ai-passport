#pragma once

#include "esp_err.h"
#include "suite_app.h"
#include "suite_translation_core.h"
#include "suite_translation_provider.h"
#include "suite_translation_tls_diag.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * In-firmware translation page/worker state.
 *
 * Real mode captures bounded microphone audio and uses the configured
 * recognition and text-translation services through authenticated HTTPS.
 * The explicit local mock mode supplies synthetic input/results for tests.
 * Workers own protocol metadata and slow audio/storage/network operations;
 * they never touch LVGL. UI callbacks only post nonblocking intents.
 */

#define SUITE_TRANSLATION_TTL_MS 120000

typedef enum {
    SUITE_TRANSLATION_IDLE = 0,
    SUITE_TRANSLATION_WORKING,
    SUITE_TRANSLATION_RECORDING,
    SUITE_TRANSLATION_DONE,
    SUITE_TRANSLATION_FAILED,
    SUITE_TRANSLATION_CANCELLED,
    SUITE_TRANSLATION_EXPIRED,
    SUITE_TRANSLATION_UNKNOWN,
    SUITE_TRANSLATION_BUSY,
    SUITE_TRANSLATION_UNAVAILABLE
} suite_translation_state_t;

typedef enum {
    SUITE_TRANSLATION_MODE_MOCK = 0,
    SUITE_TRANSLATION_MODE_REAL = 1
} suite_translation_mode_t;

typedef struct {
    suite_translation_state_t state;
    tr_direction_t direction;
    suite_translation_mode_t mode;
    bool mock;
    bool worker_busy;
    uint32_t capture_seconds;
    unsigned serial;
    unsigned session;
    unsigned identity_epoch;
    uint32_t core_generation;
    int64_t expires_at_ms;
    char source[TR_TEXT_MAX_BYTES + 1U];
    char target[TR_TEXT_MAX_BYTES + 1U];
    char operation_id[TR_ID_SIZE];
    char job_id[TR_ID_SIZE];
    uint32_t server_generation;
    suite_translation_tls_diag_t tls_diag;
    char status[96];
} suite_translation_view_t;

/* Idempotent runtime start; safe to call from page entry. */
esp_err_t suite_translation_start(void);

/* Page-session boundary. Exit invalidates late results without waiting. */
void suite_translation_session_enter(void);
void suite_translation_session_exit(void);

/* Select the explicit direction/mode. Nonblocking; rejected while a worker runs. */
bool suite_translation_select(tr_direction_t direction);
bool suite_translation_select_mode(suite_translation_mode_t mode);

/* Dispatch the selected mode. Nonblocking; only one worker may own cleanup. */
bool suite_translation_run(void);

/* Recording controls for the real mode; nonblocking intents. */
void suite_translation_finish_recording(void);
bool suite_translation_busy(void);

/* Request cancellation; the worker acknowledges cleanup asynchronously. */
void suite_translation_cancel(void);

/* Return a terminal view to ready/direction selection without starting work. */
void suite_translation_ready(void);

/*
 * Explicit local-mock-only capacity recovery. Allowed only while no worker owns
 * cleanup and the metadata table is full. It reinitializes only this volatile
 * in-firmware mock core and advances the identity epoch; production/companion
 * deduplication and durable stores are untouched.
 */
bool suite_translation_reset_mock_session(void);

/* Observe the RAM-only result TTL and clear expired text. */
void suite_translation_poll(void);

/* Replaceable mock/provider boundary; NULL restores the built-in mock. */
bool suite_translation_set_provider(const suite_translation_provider_t *provider);

/* Nonblocking snapshot borrow. Caller releases immediately; no I/O. */
const suite_translation_view_t *suite_translation_view_acquire(void);
void suite_translation_view_release(void);

#ifdef SUITE_TRANSLATION_TEST
/* Host-test-only allocation injection; absent from production firmware. */
void suite_translation_test_fail_alloc(bool fail);
/* Fail the real-worker json/source/target allocation by bits 1/2/4. */
void suite_translation_test_fail_alloc_mask(unsigned mask);
#endif

extern const suite_page_t SUITE_PAGE_TRANSLATION;
