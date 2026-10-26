#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TASK_MAX_ITEMS 200
#define TASK_RECORD_MAX 8192
#define TASK_PAGE_MAX 32768
#define TASK_CACHE_MAX (1024U * 1024U)
#define TASK_SLOT_SIZE 0x118000U
#define TASK_DATA_START 0x1000U
#define TASK_SCRATCH (TASK_SLOT_SIZE - TASK_PAGE_MAX)

/* One owner performs all Flash I/O. UI consumes a decoded model only. */
typedef struct {
    void *ctx;
    bool (*read)(void *, uint32_t, void *, size_t);
    bool (*write)(void *, uint32_t, const void *, size_t);
    bool (*erase)(void *, uint32_t, size_t);
} task_io_t;

typedef struct {
    uint32_t magic, version, generation, count, bytes, crc;
    uint32_t offsets[TASK_MAX_ITEMS];
    uint16_t lengths[TASK_MAX_ITEMS];
    int64_t server_at, device_at, selection;
    char revision[33];
    uint8_t stale, auth;
    uint8_t reserved[5];
    uint32_t header_crc;
} task_header_t;

typedef struct {
    task_io_t io;
    task_header_t active, pending;
    int slot, staging;
    bool valid, failed;
    uint32_t running_crc;
} task_cache_t;

typedef struct {
    char summary[513], note[4097], status[16], collection[65];
    char task_id[65],etag[257];
    char due_kind[12], due_value[65], due_display[65], due_zone[129];
    bool truncated, recurring, parent,completable;
} task_record_t;

typedef struct {
    char revision[33], state[16];
    int64_t server_at, selection;
    uint32_t offset, total, next, count;
    bool last;
} task_page_t;

bool task_cache_open(task_cache_t *cache, task_io_t io);
bool task_cache_begin(task_cache_t *cache);
/* Parse a complete page from staging scratch. Errors poison this transaction. */
bool task_cache_page(task_cache_t *cache, size_t bytes, task_page_t *page,
                     task_record_t *workspace);
bool task_cache_commit(task_cache_t *cache, int64_t device_at);
bool task_cache_record(task_cache_t *cache, unsigned index, task_record_t *out);
bool task_record_parse(const void *json, size_t length, task_record_t *out);
bool task_origin_valid(const char *origin);
bool task_token_valid(const char *token);
bool task_cache_stale(const task_cache_t *cache, int64_t now, bool trusted);
uint32_t task_crc32(uint32_t crc, const void *data, size_t length);
