// tests/test_suite_clock.c —— 时间可信度与历法换算主机测试。
#include <assert.h>

#include "suite_clock.h"

int main(void) {
    suite_time_tracker_t t;
    suite_time_tracker_init(&t);
    assert(t.state == SUITE_TIME_UNSYNCED);
    assert(!suite_time_is_trusted(&t, 1000));

    // 未同步 → 同步成功 → 可信。
    suite_time_sync_started(&t);
    assert(t.state == SUITE_TIME_SYNCING);
    suite_time_sync_failed(&t);
    assert(t.state == SUITE_TIME_UNSYNCED);

    suite_time_sync_started(&t);
    suite_time_sync_succeeded(&t, 1000);
    assert(suite_time_is_trusted(&t, 1000 + SUITE_TIME_RESYNC_WINDOW_S - 1));
    // 超窗后过期,提示重新校时,而不是伪装可用。
    assert(!suite_time_is_trusted(&t, 1000 + SUITE_TIME_RESYNC_WINDOW_S));

    // 已有基准后同步失败,旧基准仍在窗口内则仍可信。
    suite_time_sync_started(&t);
    suite_time_sync_failed(&t);
    assert(t.state == SUITE_TIME_SYNCED);
    assert(suite_time_is_trusted(&t, 2000));

    // 历法锚点:2026-10-07 00:00:00 UTC = epoch 1791331200(独立核算)。
    suite_civil_t c;
    suite_civil_from_epoch(1791331200, 0, &c);
    assert(c.year == 2026 && c.month == 10 && c.day == 7);
    assert(c.hour == 0 && c.minute == 0 && c.second == 0);

    // 星期:2026-10-07 是周三(weekday=3)。
    assert(c.weekday == 3);

    // 时区:CST UTC+8,跨午夜。
    suite_civil_t cst;
    suite_civil_from_epoch(1791331200 + 22 * 3600 + 33 * 60 + 24, 8 * 3600, &cst);
    assert(cst.year == 2026 && cst.month == 10 && cst.day == 8);
    assert(cst.hour == 6 && cst.minute == 33 && cst.second == 24);

    // 负 epoch(1969-12-31 23:59:59 UTC)。
    suite_civil_t pre;
    suite_civil_from_epoch(-1, 0, &pre);
    assert(pre.year == 1969 && pre.month == 12 && pre.day == 31);
    assert(pre.hour == 23 && pre.minute == 59 && pre.second == 59);
    assert(pre.weekday == 3);  // 1969-12-31 周三

    return 0;
}
