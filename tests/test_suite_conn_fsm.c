// tests/test_suite_conn_fsm.c —— 连接状态机主机测试。
#include <assert.h>

#include "suite_conn_fsm.h"

int main(void) {
    suite_conn_machine_t m;
    suite_conn_init(&m);
    assert(m.state == SUITE_CONN_OFF);

    // 正常路径:启动 → 关联 → 拿 IP → 在线。
    suite_conn_step(&m, SUITE_CONN_EV_START);
    assert(m.state == SUITE_CONN_CONNECTING);
    suite_conn_step(&m, SUITE_CONN_EV_WIFI_STARTED);
    assert(m.state == SUITE_CONN_WAIT_IP);
    suite_conn_step(&m, SUITE_CONN_EV_GOT_IP);
    assert(m.state == SUITE_CONN_ONLINE && m.retry_count == 0);

    // 在线掉线 → 退避;退避到期重试。
    suite_conn_step(&m, SUITE_CONN_EV_LOST);
    assert(m.state == SUITE_CONN_RETRY_WAIT && m.retry_count == 1);
    suite_conn_step(&m, SUITE_CONN_EV_START);
    assert(m.state == SUITE_CONN_CONNECTING);
    suite_conn_step(&m, SUITE_CONN_EV_WIFI_STARTED);
    suite_conn_step(&m, SUITE_CONN_EV_GOT_IP);
    assert(m.state == SUITE_CONN_ONLINE && m.retry_count == 0);

    // 掉线后退避,重试再次认证失败(错密码):计数累加。
    suite_conn_step(&m, SUITE_CONN_EV_LOST);
    suite_conn_step(&m, SUITE_CONN_EV_START);
    suite_conn_step(&m, SUITE_CONN_EV_ASSOC_FAIL);
    assert(m.state == SUITE_CONN_RETRY_WAIT && m.retry_count == 2);

    // 停止:清零回到 OFF(保留 NVS 配置由设备层负责)。
    suite_conn_step(&m, SUITE_CONN_EV_STOP);
    assert(m.state == SUITE_CONN_OFF && m.retry_count == 0);

    // 退避序列:1s,2s,4s,...封顶 60s。
    assert(suite_conn_backoff_ms(0) == 1000);
    assert(suite_conn_backoff_ms(1) == 2000);
    assert(suite_conn_backoff_ms(2) == 4000);
    assert(suite_conn_backoff_ms(5) == 32000);
    assert(suite_conn_backoff_ms(6) == 60000);  // 封顶
    assert(suite_conn_backoff_ms(20) == 60000);

    return 0;
}
