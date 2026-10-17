// main/suite_conn_fsm.c —— 连接状态机实现(纯逻辑)。
#include "suite_conn_fsm.h"

#define SUITE_CONN_BACKOFF_MAX_MS (60u * 1000u)

void suite_conn_init(suite_conn_machine_t *m)
{
    m->state = SUITE_CONN_OFF;
    m->retry_count = 0;
    m->last_reason = 0;
}

uint32_t suite_conn_backoff_ms(uint32_t retry_count)
{
    uint32_t ms = 1000u;
    for (uint32_t i = 0; i < retry_count && ms < SUITE_CONN_BACKOFF_MAX_MS; i++) {
        ms *= 2u;
    }
    return ms > SUITE_CONN_BACKOFF_MAX_MS ? SUITE_CONN_BACKOFF_MAX_MS : ms;
}

void suite_conn_step_lost(suite_conn_machine_t *m, uint16_t reason)
{
    m->last_reason = reason;
    suite_conn_step(m, SUITE_CONN_EV_LOST);
}

void suite_conn_step(suite_conn_machine_t *m, suite_conn_event_t ev)
{
    // 新凭据立即应用:任何状态都回到 CONNECTING 并清零失败计数与原因。
    // 设备层在此事件中断开/停止旧链路,再以新凭据重新启动连接。
    if (ev == SUITE_CONN_EV_APPLY_NEW) {
        m->state = SUITE_CONN_CONNECTING;
        m->retry_count = 0;
        m->last_reason = 0;
        return;
    }

    switch (m->state) {
    case SUITE_CONN_OFF:
        if (ev == SUITE_CONN_EV_START) {
            m->state = SUITE_CONN_CONNECTING;
        }
        break;

    case SUITE_CONN_CONNECTING:
        if (ev == SUITE_CONN_EV_WIFI_STARTED) {
            m->state = SUITE_CONN_WAIT_IP;
        } else if (ev == SUITE_CONN_EV_GOT_IP) {
            // 栈已启动时的重试不会再有 STA_START:关联/DHCP 成功后的
            // GOT_IP 直接从 CONNECTING 进入在线,否则会永远停在连接态。
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_ONLINE;
        } else if (ev == SUITE_CONN_EV_ASSOC_FAIL ||
                   ev == SUITE_CONN_EV_LOST ||
                   ev == SUITE_CONN_EV_INIT_FAIL) {
            // DISCONNECTED 可能早于 STA_STARTED 到达,与 ASSOC_FAIL 同处理。
            // 栈初始化失败同样进入退避等待:重试会重新执行初始化,而不会
            // 停在 CONNECTING 无限等待永远不会到来的启动事件。
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_WAIT_IP:
        if (ev == SUITE_CONN_EV_GOT_IP) {
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_ONLINE;
        } else if (ev == SUITE_CONN_EV_ASSOC_FAIL) {
            // esp_wifi_connect() 直接失败(无凭据/参数错误):与掉线一样
            // 进入退避重试,不能停在 WAIT_IP 无限等待 GOT_IP。
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_LOST) {
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_ONLINE:
        if (ev == SUITE_CONN_EV_LOST) {
            m->retry_count += 1;
            m->state = SUITE_CONN_RETRY_WAIT;
        } else if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_OFF;
        }
        break;

    case SUITE_CONN_RETRY_WAIT:
        if (ev == SUITE_CONN_EV_STOP) {
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_OFF;
        } else if (ev == SUITE_CONN_EV_START) {
            // 退避计时器到期重试。
            m->state = SUITE_CONN_CONNECTING;
        } else if (ev == SUITE_CONN_EV_GOT_IP) {
            // 掉线后的立即重连在退避到期前成功:直接恢复在线,
            // 不等下一次退避计时再"重连"一个已经连上的接口。
            m->retry_count = 0;
            m->last_reason = 0;
            m->state = SUITE_CONN_ONLINE;
        }
        break;

    default:
        m->state = SUITE_CONN_OFF;
        break;
    }
}
