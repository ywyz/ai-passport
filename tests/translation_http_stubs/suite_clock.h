#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SUITE_TIME_UNSYNCED = 0,
    SUITE_TIME_SYNCING,
    SUITE_TIME_SYNCED
} suite_time_state_t;

typedef struct {
    suite_time_state_t state;
    int64_t last_sync_epoch;
} suite_time_tracker_t;

bool suite_time_is_trusted(const suite_time_tracker_t *tracker,
                           int64_t now_epoch);
