#pragma once

#include "suite_translation_core.h"

#include <stddef.h>

/*
 * Replaceable external-I/O boundary for the translation worker.
 *
 * The in-firmware implementation is suite_translation_mock_provider, a
 * synthetic local provider. A later real ASR/MT adapter may implement the same
 * interface; the worker, cancellation, generation and UI paths stay unchanged.
 * A provider must be bounded and must not access LVGL, the translation core or
 * shared module state. It writes NUL-terminated UTF-8 text into the supplied
 * buffers and reports failure without truncating or inventing a translation.
 */
typedef enum {
    SUITE_TR_PROVIDER_OK = 0,
    SUITE_TR_PROVIDER_RECOGNIZE_FAILED,
    SUITE_TR_PROVIDER_TRANSLATE_FAILED
} suite_translation_provider_result_t;

typedef struct {
    const char *name;
    suite_translation_provider_result_t (*run)(tr_direction_t direction,
                                               char *source,
                                               size_t source_cap,
                                               char *target,
                                               size_t target_cap);
} suite_translation_provider_t;

extern const suite_translation_provider_t suite_translation_mock_provider;
