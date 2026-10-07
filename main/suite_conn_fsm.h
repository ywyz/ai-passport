// main/suite_conn_fsm.h —— Wi-Fi/校时连接状态机(纯逻辑,可主机测试)。
// 依据 suite-contract SC-03:断线重连、失败退避,错误不破坏既有可用配置。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SUITE_CONN_OFF = 0,      // 未启动/已停止
    SUITE_CONN_CONNECTING,   // esp_wifi_start→connect 进行中
    SUITE_CONN_WAIT_IP,      // 已关联 AP,等待 DHCP
    SUITE_CONN_ONLINE,       // 拿到 IP,可发起 SNTP/HTTPS
    SUITE_CONN_RETRY_WAIT,   // 失败退避等待,之后重试
} suite_conn_state_t;

typedef enum {
    SUITE_CONN_EV_START = 0,     // 应用要求开始连接
    SUITE_CONN_EV_STOP,          // 应用要求停止(保留 NVS 配置)
    SUITE_CONN_EV_WIFI_STARTED,  // esp_wifi_start 完成
    SUITE_CONN_EV_ASSOC_FAIL,    // 关联/认证失败(错密码等)
    SUITE_CONN_EV_GOT_IP,        // STA 拿到 IP
    SUITE_CONN_EV_LOST,          // 掉线/丢 IP
} suite_conn_event_t;

typedef struct {
    suite_conn_state_t state;
    uint32_t retry_count;        // 连续失败次数(成功后清零)
} suite_conn_machine_t;

void suite_conn_init(suite_conn_machine_t *m);
void suite_conn_step(suite_conn_machine_t *m, suite_conn_event_t ev);

// 指数退避:1s,2s,4s,...封顶 60s。retry_count 从 0 起。
uint32_t suite_conn_backoff_ms(uint32_t retry_count);
