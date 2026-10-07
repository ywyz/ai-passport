// main/suite_clock.h —— 时间可信度与历法换算(纯逻辑,可主机测试)。
// 依据 suite-plan 第 7 节:时钟不可信需独立提示;SNTP 同步成功才算可信,
// 超过重同步窗口后降级为"过期",提示重新校时而不是伪装可用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SUITE_TIME_UNSYNCED = 0,   // 开机后从未同步成功
    SUITE_TIME_SYNCING,        // 已发起 SNTP,等待结果
    SUITE_TIME_SYNCED,         // 有可信基准时间
} suite_time_state_t;

typedef struct {
    suite_time_state_t state;
    int64_t last_sync_epoch;   // 最近一次同步成功的时刻(设备 epoch 秒)
} suite_time_tracker_t;

void suite_time_tracker_init(suite_time_tracker_t *t);

void suite_time_sync_started(suite_time_tracker_t *t);
void suite_time_sync_succeeded(suite_time_tracker_t *t, int64_t now_epoch);
void suite_time_sync_failed(suite_time_tracker_t *t);

// 可信 = 已同步且距上次同步不超过重同步窗口。
// 超窗后返回 false(时间"过期",应重新校时),而不是永远可信。
bool suite_time_is_trusted(const suite_time_tracker_t *t, int64_t now_epoch);

// 重同步窗口(秒):24 h 内必须成功重同步一次,否则时间视为过期。
#define SUITE_TIME_RESYNC_WINDOW_S ((int64_t)24 * 3600)

// 历法:epoch(UTC 秒) + 时区偏移秒 → 当地年月日/时分秒/星期。
// weekday: 0=周日..6=周六。支持负 epoch(1970 前)与任意偏移。
typedef struct {
    int year;      // 完整年份,如 2026
    int month;     // 1..12
    int day;       // 1..31
    int hour;      // 0..23
    int minute;    // 0..59
    int second;    // 0..59
    int weekday;   // 0=周日..6=周六
} suite_civil_t;

void suite_civil_from_epoch(int64_t epoch_utc, int32_t tz_offset_s,
                            suite_civil_t *out);
