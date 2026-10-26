#pragma once

#include "suite_clock.h"

typedef enum {
    SUITE_CONN_OFFLINE = 0,
    SUITE_CONN_ONLINE
} suite_conn_state_t;

suite_conn_state_t suite_connection_state(void);
int64_t suite_connection_now(void);
const suite_time_tracker_t *suite_connection_time(void);
