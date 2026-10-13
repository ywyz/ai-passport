// tests/test_suite_weather_runtime.c —— 天气任务与响应收集的主机测试。
//
// 直接运行 main/suite_weather.c 的真实实现:RTOS 边界用可恢复任务桩
// (tests/weather_stubs/weather_runtime.c),时间、连接状态、城市配置与
// HTTP 响应分块全部由测试脚本控制。断言对象是行为——抓取次数、请求节拍、
// 公开快照内容与 HTTP 资源释放——而不是源码字符串。
#include "weather_stubs/weather_runtime.c"
#include "../main/suite_weather.c"

#include <stdio.h>
#include <string.h>

// 固定测试时间基准:2026-10-07 00:00:00 UTC。
#define TEST_EPOCH_BASE ((int64_t)1791331200)

static int64_t s_now = TEST_EPOCH_BASE;
static suite_time_tracker_t s_tracker;
static bool s_online = true;
static suite_city_t s_city = {
    .name = "Beijing",
    .lat_x1e4 = 395427,
    .lon_x1e4 = 1163916,
    .tz_off_s = 28800,
};
static esp_err_t s_city_result = ESP_OK;
static unsigned s_hook_calls;
static unsigned s_mid_fetch_requests;

int64_t suite_connection_now(void) { return s_now; }
const suite_time_tracker_t *suite_connection_time(void) { return &s_tracker; }
suite_conn_state_t suite_connection_state(void)
{
    return s_online ? SUITE_CONN_ONLINE : SUITE_CONN_RETRY_WAIT;
}
esp_err_t suite_storage_city_get(suite_city_t *out)
{
    if (s_city_result != ESP_OK) return s_city_result;
    *out = s_city;
    return ESP_OK;
}

static void clock_trusted(void)
{
    suite_time_sync_succeeded(&s_tracker, s_now);
}

static void clock_untrusted(void)
{
    suite_time_tracker_init(&s_tracker);
    suite_time_sync_started(&s_tracker);
}

// 与任务请求的等待一致地推进时间,并唤醒它(等待超时)。
static unsigned long weather_tick(void)
{
    s_now += (int64_t)(test_task_wait_ms() / 1000);
    return test_task_resume_timeout();
}

// 每次抓取结束(含失败与超限)都必须释放 HTTP 客户端。
static void expect_http_balanced(void)
{
    assert(!test_http_client_live());
    assert(test_http_init_calls() == test_http_cleanup_calls());
}

static bool weather_snapshot(suite_weather_data_t *out)
{
    memset(out, 0, sizeof(*out));
    return suite_weather_get(out);
}

// 正常响应:字段与 fresh 检查一致,任务随后恢复 30 min 周期。
static const char *k_ok_body =
    "{\"latitude\":39.9,\"current_units\":{\"temperature_2m\":\"°C\","
    "\"weather_code\":\"wmo code\"},\"current\":{\"time\":\"2026-10-07T08:00\","
    "\"interval\":900,\"temperature_2m\":18.2,\"weather_code\":3}}";

// 抓取过程中只投递一次的"用户再次请求刷新"钩子。
static void request_refresh_mid_fetch(void)
{
    s_hook_calls++;
    if (s_mid_fetch_requests == 0) {
        assert(suite_weather_request_refresh());
        s_mid_fetch_requests = 1;
    }
}

// 构造长度恰为 total 字节的合法响应:必需字段在前,尾部用字符串填充。
static const char *big_body(size_t total)
{
    static char buf[SUITE_WX_BODY_MAX + 1];
    const char *prefix =
        "{\"current\":{\"temperature_2m\":77.7,\"weather_code\":99,\"pad\":\"";
    const char *suffix = "\"}}";
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    assert(total > prefix_len + suffix_len && total <= SUITE_WX_BODY_MAX);
    memcpy(buf, prefix, prefix_len);
    memset(buf + prefix_len, 'x', total - prefix_len - suffix_len);
    memcpy(buf + total - suffix_len, suffix, suffix_len);
    buf[total] = '\0';
    return buf;
}

int main(void)
{
    suite_weather_data_t d;

    // 启动前:快照与刷新请求都不可用。
    assert(!suite_weather_get(&d));
    assert(!suite_weather_request_refresh());

    clock_trusted();
    test_http_reset();
    test_http_add_chunk(k_ok_body, strlen(k_ok_body));
    assert(suite_weather_start() == ESP_OK);

    // 在线且时间可信时首轮即抓取,并核对城市 → URL 的连接。
    unsigned long wait = test_task_start();
    assert(test_http_performs() == 1);
    expect_http_balanced();
    assert(strstr(test_http_last_url(), "https://api.open-meteo.com") != NULL);
    assert(strstr(test_http_last_url(), "latitude=39.5427") != NULL);
    assert(strstr(test_http_last_url(), "longitude=116.3916") != NULL);
    assert(wait == 1800000);  // 成功后 30 min 周期
    assert(weather_snapshot(&d));
    assert(d.temperature > 18.19 && d.temperature < 18.21);
    assert(d.wmo_code == 3);
    assert(!d.stale);

    // 正常周期:下一次唤醒仍是完整 30 min,并再次抓取。
    wait = weather_tick();
    assert(test_http_performs() == 2);
    expect_http_balanced();
    assert(wait == 1800000);

    // 手动刷新:唤醒只提前一次抓取,完成后恢复等待周期,不跳过等待间隔。
    assert(suite_weather_request_refresh());
    wait = test_task_resume_wake();
    assert(test_http_performs() == 3);
    expect_http_balanced();
    assert(wait == 1800000);

    // 连续两次请求刷新(尚未唤醒)合并为一次抓取,唤醒后无遗留令牌,
    // 因此随后的等待是完整周期而不是"零延时"的再次抓取。
    assert(suite_weather_request_refresh());
    assert(suite_weather_request_refresh());
    wait = test_task_resume_wake();
    assert(test_http_performs() == 4);
    expect_http_balanced();
    assert(wait == 1800000);
    wait = weather_tick();
    assert(test_http_performs() == 5);
    assert(wait == 1800000);

    // 抓取过程中到达的刷新请求不能因清零而丢失:响应分块投递期间请求刷新,
    // 本轮抓取结束后必须立即再抓取一次。
    s_hook_calls = 0;
    s_mid_fetch_requests = 0;
    test_http_on_chunk(request_refresh_mid_fetch);
    wait = weather_tick();
    assert(s_mid_fetch_requests == 1);
    assert(s_hook_calls == 2);          // 本轮与补做的一轮各调用一次
    assert(test_http_performs() == 7);  // 本轮 + 立即补做的一轮
    expect_http_balanced();
    assert(wait == 1800000);
    test_http_on_chunk(NULL);
    // 补做的抓取没有再次投递请求,后续回到正常周期。
    wait = weather_tick();
    assert(test_http_performs() == 8);
    assert(wait == 1800000);

    // 失败退避:传输失败 → 60 s 重试,旧数据保留,过期状态按旧时间计算。
    test_http_set_result(ESP_FAIL);
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d));
    assert(d.temperature > 18.19 && d.temperature < 18.21);
    assert(d.wmo_code == 3);

    // HTTP 429(服务商限流)同样按失败退避处理,不覆盖旧数据。
    test_http_set_result(ESP_OK);
    test_http_set_status(429);
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d) && d.wmo_code == 3);

    // 空响应体(HTTP 200 但无内容)按解析失败处理,旧数据保留。
    test_http_reset();
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d) && d.temperature > 18.19 && d.temperature < 18.21);

    // 旧数据超过 2 h 新鲜窗口后:保留但仍可用,过期状态独立呈现。
    s_now += SUITE_WX_FRESH_WINDOW_S;
    assert(weather_snapshot(&d));
    assert(d.stale);

    // 超限响应:整次失败,即使必需字段在开头也不更新数据。
    test_http_reset();
    const char *one_byte_too_long = big_body(SUITE_WX_BODY_MAX);
    test_http_add_chunk(one_byte_too_long, SUITE_WX_BODY_MAX);
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d));
    assert(d.temperature > 18.19 && d.temperature < 18.21);  // 不是 77.7
    assert(d.wmo_code == 3);                                 // 不是 99
    assert(d.stale);  // 旧数据仍可用,过期状态按旧抓取时间独立计算

    // 最大合法长度仍成功:每次请求重新初始化收集状态(上次超限不泄漏)。
    test_http_reset();
    const char *max_legal = big_body(SUITE_WX_BODY_MAX - 1);
    test_http_add_chunk(max_legal, SUITE_WX_BODY_MAX - 1);
    wait = weather_tick();
    assert(wait == 1800000);
    expect_http_balanced();
    assert(weather_snapshot(&d));
    assert(d.temperature > 77.69 && d.temperature < 77.71);
    assert(d.wmo_code == 99);
    assert(!d.stale);

    // 多个分块累计超限:单块都不超,累计超 1 字节即整次失败。
    test_http_reset();
    const char *chunk_a = big_body(SUITE_WX_BODY_MAX - 1);
    test_http_add_chunk(chunk_a, 5000);
    test_http_add_chunk(chunk_a + 5000, SUITE_WX_BODY_MAX - 5000);  // 累计 8192
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d));
    assert(d.temperature > 77.69 && d.temperature < 77.71);  // 仍是上一次有效值
    assert(d.wmo_code == 99);

    // 必需字段位于开头、后续分块超限:整次失败而不是"解析成功"。
    test_http_reset();
    const char *field_prefix =
        "{\"current\":{\"temperature_2m\":-11.5,\"weather_code\":71,\"pad\":\"";
    test_http_add_chunk(field_prefix, strlen(field_prefix));
    test_http_add_chunk(chunk_a + strlen(field_prefix),
                        SUITE_WX_BODY_MAX - strlen(field_prefix));
    wait = weather_tick();
    assert(wait == 60000);
    expect_http_balanced();
    assert(weather_snapshot(&d));
    assert(d.temperature > 77.69 && d.temperature < 77.71);  // 不是 -11.5
    assert(d.wmo_code == 99);                                // 不是 71

    // HTTP 客户端初始化失败:按失败退避,不产生抓取。
    test_http_reset();
    test_http_set_init_fails(true);
    wait = weather_tick();
    assert(wait == 60000);
    assert(test_http_performs() == 0);
    assert(test_http_init_fail_calls() == 1);
    expect_http_balanced();
    test_http_set_init_fails(false);

    // 城市配置读取失败:不发起无效请求,按未就绪节拍复查。
    s_city_result = ESP_ERR_NOT_FOUND;
    wait = weather_tick();
    assert(wait == 2000);
    assert(test_http_performs() == 0);
    expect_http_balanced();
    s_city_result = ESP_OK;

    // 离线:每 2 s 复查且不抓取,不会忙循环;刷新请求被拒绝。
    s_online = false;
    assert(!suite_weather_request_refresh());
    for (int i = 0; i < 3; i++) {
        wait = weather_tick();
        assert(wait == 2000);
    }
    assert(test_http_performs() == 0);
    expect_http_balanced();

    // 未校时(离线恢复但时间不可信):同样是 2 s 复查,不抓取。
    s_online = true;
    clock_untrusted();
    assert(!suite_weather_request_refresh());
    wait = weather_tick();
    assert(wait == 2000);
    assert(test_http_performs() == 0);

    // 时间重新可信:恢复抓取,并回到 30 min 周期。
    clock_trusted();
    test_http_reset();
    test_http_add_chunk(k_ok_body, strlen(k_ok_body));
    wait = weather_tick();
    assert(test_http_performs() == 1);
    assert(wait == 1800000);
    expect_http_balanced();

    // 停止状态:刷新 API 拒绝请求,任务被唤醒后退出。
    suite_weather_stop();
    assert(!suite_weather_request_refresh());
    wait = test_task_resume_wake();
    assert(test_task_finished());
    assert(wait == 0);
    expect_http_balanced();

    puts("weather scheduling and bounded-response tests: PASS");
    return 0;
}
