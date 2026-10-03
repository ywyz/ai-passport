/**
 * passport_kv.h - tiny durable key-value seam for the travel application.
 *
 * Firmware binds this seam to nvs (records partition namespace "trv").
 * Host tests bind it to an in-memory or file-backed map so journal rules,
 * pointer recovery and acknowledgment semantics are testable without
 * ESP-IDF (DR04 crash-consistency properties).
 */
#ifndef PASSPORT_KV_H
#define PASSPORT_KV_H

#include <stddef.h>

#define PASSPORT_KV_MAX_KEY 32
#define PASSPORT_KV_MAX_VALUE 1024

struct passport_kv {
    int (*get)(void *ctx, const char *key, void *out, size_t max_len,
               size_t *out_len);
    int (*set)(void *ctx, const char *key, const void *data, size_t len);
    int (*del)(void *ctx, const char *key);
    /* enumerate existing keys with a prefix; callback returns non-zero stop */
    int (*list)(void *ctx, const char *prefix,
                int (*cb)(void *cbctx, const char *key), void *cbctx);
    void *ctx;
};

#endif /* PASSPORT_KV_H */
