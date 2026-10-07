// main/suite_clock.c —— 时间可信度与历法换算实现(纯逻辑)。
#include "suite_clock.h"

void suite_time_tracker_init(suite_time_tracker_t *t)
{
    t->state = SUITE_TIME_UNSYNCED;
    t->last_sync_epoch = 0;
}

void suite_time_sync_started(suite_time_tracker_t *t)
{
    t->state = SUITE_TIME_SYNCING;
}

void suite_time_sync_succeeded(suite_time_tracker_t *t, int64_t now_epoch)
{
    t->state = SUITE_TIME_SYNCED;
    t->last_sync_epoch = now_epoch;
}

void suite_time_sync_failed(suite_time_tracker_t *t)
{
    // 同步失败回到未同步;若此前已有可信基准,可信性由 is_trusted 按窗口判断。
    if (t->state == SUITE_TIME_SYNCING) {
        t->state = (t->last_sync_epoch > 0) ? SUITE_TIME_SYNCED : SUITE_TIME_UNSYNCED;
    }
}

bool suite_time_is_trusted(const suite_time_tracker_t *t, int64_t now_epoch)
{
    if (t->state != SUITE_TIME_SYNCED || t->last_sync_epoch <= 0) return false;
    return (now_epoch - t->last_sync_epoch) < SUITE_TIME_RESYNC_WINDOW_S;
}

// days → civil 算法(Howard Hinnant 版),无符号除法处理负 epoch。
void suite_civil_from_epoch(int64_t epoch_utc, int32_t tz_offset_s,
                            suite_civil_t *out)
{
    int64_t days = (epoch_utc + tz_offset_s) / 86400;
    int64_t secs = (epoch_utc + tz_offset_s) % 86400;
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    out->weekday = (int)((days + 4) % 7);          // 1970-01-01 是周四(=4)
    if (out->weekday < 0) out->weekday += 7;

    int64_t z = days + 719468;                     // 转为以 3 月为元年的历元
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                                    // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
    int64_t m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
    if (m <= 2) y += 1;

    out->year = (int)y;
    out->month = (int)m;
    out->day = (int)d;
    out->hour = (int)(secs / 3600);
    out->minute = (int)((secs % 3600) / 60);
    out->second = (int)(secs % 60);
}
