// main/suite_connection.h —— Wi-Fi STA + SNTP 校时设备侧管理。
// 依据 SC-03:上线后才发起 SNTP/HTTPS;断线停止校时并退避重连;
// 凭据保存在 NVS,连接失败不改动既有配置。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#include "suite_clock.h"
#include "suite_conn_fsm.h"

// 幂等启动:初始化 netif/event loop/Wi-Fi STA 并开始连接循环。
// 首次调用后重复调用返回 ESP_OK。
esp_err_t suite_connection_start(void);

// 停止连接循环与 SNTP(保留 NVS 凭据)。主要供测试/降级使用。
void suite_connection_stop(void);

suite_conn_state_t suite_connection_state(void);

// 当前 epoch 秒(本地 time(NULL));tracker 描述可信度。
int64_t suite_connection_now(void);
const suite_time_tracker_t *suite_connection_time(void);

// 手动触发一次立即重连(状态页 OK 键)。在线时等效无操作。
void suite_connection_reconnect_now(void);
