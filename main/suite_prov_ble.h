// main/suite_prov_ble.h —— 手机扫码 BLE 配网:NimBLE GATT 服务与协议任务。
//
// 自定义 128-bit GATT 服务(网页与设备共用,规范见
// provisioning/web/README.md):
//   Service 7a110001-4f1a-8c20-50a7-b6e900000001
//   RX(写)  7a110002-4f1a-8c20-50a7-b6e900000002  手机→设备 JSON 帧
//   TX(通知)7a110003-4f1a-8c20-50a7-b6e900000003  设备→手机 JSON 帧
//   Info(读)7a110004-4f1a-8c20-50a7-b6e900000004  {"id","fw"} 静态信息
// 广播名 FoloPassport-XXXX,XXXX 为 BLE MAC 末两字节大写十六进制,
// 同时是配网网址 ?d=XXXX 里的设备 ID。
//
// 安全基线:链路层 Just Works(无 MITM 防护),授权依赖应用层 6 位配对码;
// 这是基础防护,不是完整的授权/加密设计。配对码只上屏,不写日志。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "suite_conn_fsm.h"

typedef enum {
    SUITE_PROV_PHASE_IDLE = 0,      // 配网页未打开,NimBLE host 常驻但不广播
    SUITE_PROV_PHASE_ADVERTISING,   // 广播中,等待手机连接
    SUITE_PROV_PHASE_CONNECTED,     // 手机已连接,尚未通过配对码
    SUITE_PROV_PHASE_AUTHED,        // 配对码通过,可收发配网指令
} suite_prov_phase_t;

// 配网页 250ms 轮询用的状态快照。
typedef struct {
    suite_prov_phase_t phase;
    suite_conn_state_t wifi;
    uint16_t reason;       // 最近一次断开原因(0=无)
    bool has_ip;
    char ip[16];
    int rssi;              // 0 表示未关联
    bool time_synced;
    char city[32];
} suite_prov_snapshot_t;

// app_main 调用一次:初始化 NimBLE host 与 GATT 服务(之后常驻)。
esp_err_t suite_prov_ble_init(void);

// 进入配网页:登记 6 位配对码、开始可连接广播、启动协议处理任务。
// 重复调用返回 ESP_ERR_INVALID_STATE。
esp_err_t suite_prov_ble_begin(const char pin[6]);

// 退出配网页(幂等):断开手机连接、停止广播、停止并删除协议任务。
void suite_prov_ble_end(void);

// 采集当前状态快照供屏幕状态行使用(内部自取所需锁,不访问 LVGL)。
void suite_prov_ble_snapshot(suite_prov_snapshot_t *out);

// 设备短 ID(4 个大写十六进制字符 + '\0');host 未同步时返回 false。
bool suite_prov_ble_device_id(char out[5]);
