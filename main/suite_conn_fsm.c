// main/suite_conn_fsm.c —— 连接状态机实现(纯逻辑)。
#include "suite_conn_fsm.h"

#define SUITE_CONN_BACKOFF_MAX_MS (60u * 1000u)

void suite_conn_init(suite_conn_machine_t *m)
{
    m->state = SUITE_CONN_OFF;
    m->retry_count = 0;
}

uint32_t suite_conn_backoff_ms(uint32_t retry_count)
{
    uint32_t ms = 1000u;
    for (uint32_t i = 0; i < retry_count && ms < SUITE_CONN_BACKOFF_MAX_MS; i++) {
        ms *= 2u;
    }
    return ms > SUITE_CONN_BACKOFF_MAX_MS ? SUITE_CONN_BACKOFF_MAX_MS : ms;
}

void suite_conn_step(suite_conn_machine_t *m, suite_conn_event_t ev)
{
    switch (m->state) {
    case SUITE_CONN_OFF:
        if (ev == SUITE_CONN_EV_START) {
            m->state = SUITE_CONN_CONNECTING;
        }
        break;

    case SUITE_CONN_CONNECTING:
        if (ev == SUITE_CONN_EV_WIFI_STARTED) {
            m->state = SUITE_CONN_WAIT_IP;
        } else if (ev == SUITE_CONN_EV_ASSOC_FAIL) {
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_WAIT_IP:
        if (ev == SUITE_CONN_EV_GOT_IP) {
            m->retry_count = 0;
            m->state = SUITE_CONN_ONLINE;
        } else if (ev == SUITE_CONN_EV_LOST) {
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_ONLINE:
        if (ev == SUITE_CONN_EV_LOST) {
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_RETRY_WAIT:
        if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->state = SUITE_CONN_OFF;
        } else if (ev == SUITE_CONN_EV_START) {
            // 退避计时器到期重试。
            m->state = SUITE_CONN_CONNECTING;
        }
        break;

    default:
        m->state = SUITE_CONN_OFF;
        break;
    }
}
