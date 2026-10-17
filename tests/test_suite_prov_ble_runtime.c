// tests/test_suite_prov_ble_runtime.c —— 配网 BLE 跨连接隔离回归。
//
// 直接运行 main/suite_prov_ble.c 的真实实现:prov_task 在真实线程上停靠于
// 队列接收(prov_runtime.c),测试驱动真实的 gap_event/gatt_access/prov_task/
// 通知出口。并发交错由独立 host 线程 + 通知提交闸门构造(见"并发交错"
// 一节):host 线程按真实 NimBLE 顺序执行换机,发送线程在闸门内仍持有
// 生产生命周期锁;桩不查询生产是否持锁,危险窗口只由测试打开。
// 覆盖复审复现的边界:
//   1. 阻塞扫描期间的断开 + 同句柄重连 + 旧请求积压:旧会话的扫描结果与
//      受保护状态不得发给未 hello 的新连接(零帧);
//   2. 队列满导致 GAP 事件被丢弃:链路状态仍被代次快照记录,新会话恢复且
//      不继承旧授权;
//   3. MTU 23/247 与订阅不跨连接继承;帧长符合实际 MTU;
//   4. 多帧发送中途与提交前的链路切换:剩余帧全部放弃;实际交付的帧
//      必须是旧连接、旧代次、旧 MTU,且内容与应答一致;
//   5. 错误配对码计数与锁定断开、正确 hello 恢复正常流程;
//   6. 终校验之后、通知提交之前的断连/同句柄重连/reset(host 线程交错;
//      新手机默认 23 或自行协商 247):帧不会落到新连接(零帧,含不完整
//      片段),换机完成序号必须晚于提交通过闸门;
//   7. 扫描结果:正常内容(去重/最强/排序/上限/隐藏过滤)、空列表、
//      会话失效、aps/ents 分配失败、真实 cJSON 的关键分配/添加失败、
//      获取失败——每条路径都不遗留、不重复释放,失败不伪装成成功;
//   8. 应答外层封装(reply_ok/reply_err):root 创建失败、id/ok 字段失败、
//      data 挂载失败、序列化失败、错误帧自身失败——不发送残缺/虚假的
//      成功帧,资源与真实分配器存活块数回到基线,故障解除后恢复正常;
//   9. 配网页"空闲态"退出、重进与资源回收(队列/信号量/任务/在途 mbuf),
//      不宣称覆盖强制终止(断电/复位)路径。
//
// 帧记录按"提交时该句柄的连接序号"归属:句柄复用后旧连接与新连接序号
// 不同,因此可以断言"未认证连接收到零帧受保护数据(含不完整片段)"。
#define _DEFAULT_SOURCE  // glibc strlcpy 声明(与固件 newlib 对齐)

#include "ble_stubs/prov_runtime.c"
// 真实 cJSON(IDF 5.5.3 内嵌 1.7.19):关键用例必须经过真实实现的
// 分配/失败/所有权语义,简化替身会掩盖失败返回值与部分添加行为。
#include "ble_stubs/cJSON.c"

// ---- JSON 失败注入与分配记账(真实 cJSON + 调用点包装) ----
//
// 包装只在指定调用点上注入 NULL/false 返回值,所有真实的分配、释放与
// 所有权语义仍由 cJSON 本体执行;未武装时与直接调用完全一致。
enum {
    JSON_OP_NONE = 0,
    JSON_OP_NETS_ARRAY,    // nets/provinces/cities 的 cJSON_CreateArray
    JSON_OP_CREATE_OBJECT, // cJSON_CreateObject(结果项 / data / 应答 root)
    JSON_OP_SSID,          // cJSON_AddStringToObject(ssid/n/err/...)
    JSON_OP_ADD_NUMBER,    // cJSON_AddNumberToObject(rssi/sec/id/...)
    JSON_OP_ADD_BOOL,      // cJSON_AddBoolToObject(ok/time_synced)
    JSON_OP_ADD_ITEM_ARRAY,  // cJSON_AddItemToArray(nets,net)
    JSON_OP_ADD_ITEM_OBJECT, // cJSON_AddItemToObject(data,nets/city)
    JSON_OP_PRINT,           // 序列化缓冲:cJSON_PrintUnformatted
    JSON_OP_ALL,             // 所有被包装的调用都失败(含错误应答)
};

// 注入规则:对指定 op 的第 nth 次匹配调用返回失败(1-based)。最多同时
// 武装两条规则,用于"成功应答组装失败 + 错误应答自身也失败"的组合
// (例如 data 挂载失败后,错误帧的对象/键/序列化也分配失败)。
enum { JSON_INJECT_RULES = 2 };

typedef struct {
    int op;
    unsigned fail_at;
    unsigned calls;
} json_inject_t;

static json_inject_t s_inject[JSON_INJECT_RULES];
static bool s_json_fail_all;

static void json_reset(void)
{
    memset(s_inject, 0, sizeof(s_inject));
    s_json_fail_all = false;
}

// 第一条规则:历史用例(单点注入)。
static void test_fail_json_at(int op, unsigned nth)
{
    json_reset();
    s_inject[0].op = op;
    s_inject[0].fail_at = nth;
}

// 追加第二条规则:与前一条同时生效(计数各自独立)。
static void test_fail_json_more(int op, unsigned nth)
{
    s_inject[1].op = op;
    s_inject[1].fail_at = nth;
}

static void test_fail_all_json(bool on)
{
    json_reset();
    s_json_fail_all = on;
}

static bool json_fail(int op)
{
    if (s_json_fail_all) return true;
    for (int i = 0; i < JSON_INJECT_RULES; i++) {
        if (s_inject[i].op != op) continue;
        s_inject[i].calls++;
        if (s_inject[i].calls == s_inject[i].fail_at) return true;
    }
    return false;
}

// ---- 真实分配器记账 ----
//
// 把 cJSON 的全局钩子换成计数版 malloc/free(内部仍是 libc 分配器):
// 根对象、未挂载子树、键/值字符串、序列化缓冲全部走同一条分配路径,
// "存活块数"在命令边界可直接比较。生产用 cJSON_free 释放序列化缓冲,
// 与本记账对称;未挂钩时行为与默认 hooks 完全一致。
static size_t s_json_live;

static void *test_json_malloc(size_t size)
{
    void *p = malloc(size);
    if (p) s_json_live++;
    return p;
}

static void test_json_free(void *p)
{
    if (p) s_json_live--;
    free(p);
}

static size_t test_json_live_allocs(void)
{
    return s_json_live;
}

static void test_json_hooks_install(void)
{
    cJSON_Hooks hooks = { .malloc_fn = test_json_malloc,
                          .free_fn = test_json_free };
    cJSON_InitHooks(&hooks);
}

static cJSON *test_json_CreateArray(void)
{
    if (json_fail(JSON_OP_NETS_ARRAY)) return NULL;
    return cJSON_CreateArray();
}

static cJSON *test_json_CreateObject(void)
{
    if (json_fail(JSON_OP_CREATE_OBJECT)) return NULL;
    return cJSON_CreateObject();
}

static cJSON *test_json_AddStringToObject(cJSON *object, const char *name,
                                          const char *string)
{
    if (json_fail(JSON_OP_SSID)) return NULL;
    return cJSON_AddStringToObject(object, name, string);
}

static cJSON *test_json_AddNumberToObject(cJSON *object, const char *name,
                                          double number)
{
    if (json_fail(JSON_OP_ADD_NUMBER)) return NULL;
    return cJSON_AddNumberToObject(object, name, number);
}

static cJSON *test_json_AddBoolToObject(cJSON *object, const char *name,
                                        cJSON_bool boolean)
{
    if (json_fail(JSON_OP_ADD_BOOL)) return NULL;
    return cJSON_AddBoolToObject(object, name, boolean);
}

static cJSON_bool test_json_AddItemToArray(cJSON *array, cJSON *item)
{
    if (json_fail(JSON_OP_ADD_ITEM_ARRAY)) return false;
    return cJSON_AddItemToArray(array, item);
}

static cJSON_bool test_json_AddItemToObject(cJSON *object, const char *name,
                                            cJSON *item)
{
    if (json_fail(JSON_OP_ADD_ITEM_OBJECT)) return false;
    return cJSON_AddItemToObject(object, name, item);
}

static char *test_json_PrintUnformatted(cJSON *item)
{
    if (json_fail(JSON_OP_PRINT)) return NULL;
    return cJSON_PrintUnformatted(item);
}

// 生产 cmd_wifi_scan 的应用侧分配失败注入:只拦截生产源码里的 calloc
// (桩与真实 cJSON 已在本行之前编译,使用真实分配器)。
#define calloc test_prov_calloc
#define cJSON_CreateArray test_json_CreateArray
#define cJSON_CreateObject test_json_CreateObject
#define cJSON_AddStringToObject test_json_AddStringToObject
#define cJSON_AddNumberToObject test_json_AddNumberToObject
#define cJSON_AddBoolToObject test_json_AddBoolToObject
#define cJSON_AddItemToArray test_json_AddItemToArray
#define cJSON_AddItemToObject test_json_AddItemToObject
#define cJSON_PrintUnformatted test_json_PrintUnformatted
#include "../main/suite_prov_ble.c"
#undef cJSON_PrintUnformatted
#undef cJSON_AddItemToObject
#undef cJSON_AddItemToArray
#undef cJSON_AddBoolToObject
#undef cJSON_AddNumberToObject
#undef cJSON_AddStringToObject
#undef cJSON_CreateObject
#undef cJSON_CreateArray
#undef calloc

#include <stdio.h>
#include <string.h>

#define PARK_WAIT_MS 200UL
#define MAX_TX_FRAMES 512
#define MAX_TX_MSGS 64

// ---- 生产侧依赖的桩:连接状态 / 存储 / 城市数据 / 天气 ----

static suite_conn_state_t s_wifi_state = SUITE_CONN_ONLINE;
static const char *s_ip = "192.168.1.23";
static uint16_t s_reason;
static suite_time_tracker_t s_tracker;
static bool s_wifi_exists;
static suite_wifi_cred_t s_cred;
static suite_city_t s_saved_city;

suite_conn_state_t suite_connection_state(void) { return s_wifi_state; }

bool suite_connection_ip_str(char *out, size_t cap)
{
    if (!s_ip || !out || cap == 0) return false;
    snprintf(out, cap, "%s", s_ip);
    return true;
}

bool suite_connection_ap_rssi(int *rssi_out)
{
    if (!rssi_out) return false;
    *rssi_out = -55;
    return true;
}

const suite_time_tracker_t *suite_connection_time(void) { return &s_tracker; }
int64_t suite_connection_now(void) { return 1791331200; }
uint16_t suite_connection_last_reason(void) { return s_reason; }
esp_err_t suite_connection_apply_new_wifi(void) { return ESP_OK; }

esp_err_t suite_storage_wifi_get(suite_wifi_cred_t *out)
{
    if (!s_wifi_exists) return ESP_ERR_NOT_FOUND;
    *out = s_cred;
    return ESP_OK;
}

bool suite_storage_wifi_exists(void) { return s_wifi_exists; }

esp_err_t suite_storage_wifi_set(const suite_wifi_cred_t *in)
{
    s_cred = *in;
    s_wifi_exists = true;
    return ESP_OK;
}

esp_err_t suite_storage_city_get(suite_city_t *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->name, "Beijing");
    out->tz_off_s = 8 * 3600;
    return ESP_OK;
}

esp_err_t suite_storage_city_set(const suite_city_t *in)
{
    s_saved_city = *in;
    return ESP_OK;
}

bool suite_weather_request_refresh(void) { return true; }

// 城市桩:一个省 8 个城市,确保 cities 应答在 247 下也需多帧;
// provinces 应答约 150B,在 23 下多帧、在 247 下 1 帧,便于校验帧长契约。
static const suite_city_def_t k_city_eight[] = {
    { "CityOneLong", 100001, 200001 },   { "CityTwoLong", 100002, 200002 },
    { "CityThreeLong", 100003, 200003 }, { "CityFourLong", 100004, 200004 },
    { "CityFiveLong", 100005, 200005 },  { "CitySixLong", 100006, 200006 },
    { "CitySevenLong", 100007, 200007 }, { "CityEightLong", 100008, 200008 },
};
static const suite_city_def_t k_city_small[] = {
    { "SoloCity", 300001, 400001 },
};
static const suite_province_def_t k_provinces[] = {
    { "ProvinceAlphaLong", k_city_eight, 8 },
    { "ProvinceBetaLong", k_city_small, 1 },
    { "ProvinceGammaLong", k_city_small, 1 },
};

int suite_city_province_count(void)
{
    return (int)(sizeof(k_provinces) / sizeof(k_provinces[0]));
}

const suite_province_def_t *suite_city_province_at(int index)
{
    if (index < 0 || index >= suite_city_province_count()) return NULL;
    return &k_provinces[index];
}

// ---- 通知出口观察 ----

typedef struct {
    uint32_t gen;    // 提交时的链路代次(生产静态状态快照)
    unsigned conn;   // 提交时该句柄的连接序号(句柄复用后不同)
    unsigned att_mtu;  // 提交时该连接协商的 ATT MTU
    uint16_t handle;
    size_t len;
    uint8_t data[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];
} tx_frame_t;

static tx_frame_t g_frames[MAX_TX_FRAMES];
static size_t g_frame_count;

static void frame_sink(uint16_t conn, unsigned conn_serial, unsigned att_mtu,
                       const uint8_t *frame, size_t len)
{
    assert(g_frame_count < MAX_TX_FRAMES);
    prov_link_t link = link_snapshot();
    tx_frame_t *out = &g_frames[g_frame_count++];
    out->gen = link.gen;
    out->conn = conn_serial;
    out->att_mtu = att_mtu;
    out->handle = conn;
    out->len = len;
    assert(len <= sizeof(out->data));
    // 发送记录必须符合该连接的实际 ATT 限制:超出即生产用了别的连接的 MTU。
    assert(att_mtu >= SUITE_PROV_MTU_DEFAULT);
    assert(len <= att_mtu - 3);
    memcpy(out->data, frame, len);
}

// 把 [from,to) 区间内的帧重组为消息;START 帧开启新消息,中断的段记为不完整。
typedef struct {
    uint32_t gen;
    unsigned conn;
    bool complete;
    size_t frames;
    char text[SUITE_PROV_MSG_MAX];
} tx_msg_t;

static size_t collect_messages(size_t from, size_t to, tx_msg_t *out, size_t cap)
{
    suite_prov_reasm_t reasm;
    static uint8_t buf[SUITE_PROV_MSG_MAX];
    suite_prov_reasm_init(&reasm, buf, sizeof(buf));
    size_t n = 0;
    size_t cur_frames = 0;
    uint32_t cur_gen = 0;
    unsigned cur_conn = 0;
    for (size_t i = from; i < to; i++) {
        const tx_frame_t *frame = &g_frames[i];
        if (frame->data[0] & SUITE_PROV_FLAG_START) {
            if (cur_frames) {
                assert(n < cap);
                out[n++] = (tx_msg_t){ .gen = cur_gen, .conn = cur_conn,
                                       .complete = false,
                                       .frames = cur_frames };
            }
            cur_frames = 0;
            cur_gen = frame->gen;
            cur_conn = frame->conn;
        }
        cur_frames++;
        const uint8_t *text = NULL;
        size_t text_len = 0;
        int rc = suite_prov_reasm_feed(&reasm, frame->data, frame->len,
                                       (uint32_t)i, &text, &text_len);
        if (rc > 0) {
            assert(n < cap && text_len < sizeof(out[n].text));
            out[n] = (tx_msg_t){ .gen = cur_gen, .conn = cur_conn,
                                 .complete = true, .frames = cur_frames };
            memcpy(out[n].text, text, text_len);
            out[n].text[text_len] = '\0';
            n++;
            cur_frames = 0;
        } else if (rc < 0) {
            assert(n < cap);
            out[n++] = (tx_msg_t){ .gen = cur_gen, .conn = cur_conn,
                                   .complete = false, .frames = cur_frames };
            cur_frames = 0;
        }
    }
    if (cur_frames) {
        assert(n < cap);
        out[n++] = (tx_msg_t){ .gen = cur_gen, .conn = cur_conn,
                               .complete = false, .frames = cur_frames };
    }
    return n;
}

static size_t count_messages(const tx_msg_t *msgs, size_t n, uint32_t gen,
                             const char *needle)
{
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].complete && msgs[i].gen == gen &&
            strstr(msgs[i].text, needle)) {
            count++;
        }
    }
    return count;
}

static size_t count_messages_conn(const tx_msg_t *msgs, size_t n, unsigned conn,
                                  const char *needle)
{
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].complete && msgs[i].conn == conn &&
            strstr(msgs[i].text, needle)) {
            count++;
        }
    }
    return count;
}

// 受保护数据:扫描结果、状态推送(SSID 掩码/本地 IP/城市)与状态应答。
static bool any_protected(const tx_msg_t *msgs, size_t n, uint32_t gen)
{
    return count_messages(msgs, n, gen, "\"nets\"") > 0 ||
           count_messages(msgs, n, gen, "\"ssid\"") > 0 ||
           count_messages(msgs, n, gen, "\"ip\"") > 0 ||
           count_messages(msgs, n, gen, "\"ev\":\"status\"") > 0;
}

static bool any_protected_conn(const tx_msg_t *msgs, size_t n, unsigned conn)
{
    return count_messages_conn(msgs, n, conn, "\"nets\"") > 0 ||
           count_messages_conn(msgs, n, conn, "\"ssid\"") > 0 ||
           count_messages_conn(msgs, n, conn, "\"ip\"") > 0 ||
           count_messages_conn(msgs, n, conn, "\"ev\":\"status\"") > 0;
}

static size_t frames_with_gen(size_t from, uint32_t gen)
{
    size_t count = 0;
    for (size_t i = from; i < g_frame_count; i++) {
        if (g_frames[i].gen == gen) count++;
    }
    return count;
}

// 交付给某个连接序号的帧数:句柄复用后旧/新连接序号不同,零帧要求按
// 连接序号断言(含不完整片段,不做内容匹配)。
static size_t frames_to_conn(size_t from, unsigned conn)
{
    size_t count = 0;
    for (size_t i = from; i < g_frame_count; i++) {
        if (g_frames[i].conn == conn) count++;
    }
    return count;
}

// ---- 应答完整性软断言 ----
//
// 失败场景下不立即中止:收集并打印全部违例,场景末尾统一判定。这样
// "修复前实现"的回归能同时给出"虚假/残缺成功帧"与"资源泄漏"两类
// 证据,而不是只看到第一条断言。
static unsigned s_checks_failed;

static void check(bool ok, const char *what, const char *detail)
{
    if (ok) return;
    s_checks_failed++;
    if (detail) {
        fprintf(stderr, "ENVELOPE CHECK FAILED: %s [%s]\n", what, detail);
    } else {
        fprintf(stderr, "ENVELOPE CHECK FAILED: %s\n", what);
    }
}

static void check_zero(const char *scenario)
{
    if (s_checks_failed != 0) {
        fprintf(stderr, "%s: %u envelope check(s) failed\n", scenario,
                s_checks_failed);
    }
    assert(s_checks_failed == 0);
}

// 该代次内每条完整消息都必须自洽。用真实 cJSON 解析每条完整帧并校验
// 类型与必需字段——拒绝空对象 {}、数组/标量、以及任何字段残缺或类型
// 错误的"应答";仅检查子串(旧实现)会放过完全不含 id/ok 的 {}:
//   - 合法异步状态事件:ev="status" 字符串 + data 对象,且不带 id;
//   - ok:true 应答:带数字 id、布尔 ok,且必须有 data;
//   - ok:false 应答:带数字 id、布尔 ok,且必须有字符串 err;
//   - 带 id 却没有布尔 ok 的完整帧在此判违例。
// 解析/释放经同一套计数钩子,存活分配块数在场景检查点时不受影响。
static void check_reply_frames(const tx_msg_t *msgs, size_t n, uint32_t gen)
{
    for (size_t i = 0; i < n; i++) {
        if (!msgs[i].complete || msgs[i].gen != gen) continue;
        const char *text = msgs[i].text;
        cJSON *root = cJSON_ParseWithLength(text, strlen(text));
        if (root == NULL) {
            check(false, "完整帧不是合法 JSON", text);
            continue;
        }
        if (!cJSON_IsObject(root)) {
            check(false, "完整帧不是 JSON 对象", text);
            cJSON_Delete(root);
            continue;
        }
        const cJSON *ev = cJSON_GetObjectItem(root, "ev");
        if (ev != NULL) {
            check(cJSON_IsString(ev) &&
                      strcmp(ev->valuestring, "status") == 0,
                  "未知的异步事件类型", text);
            check(cJSON_IsObject(cJSON_GetObjectItem(root, "data")),
                  "状态事件缺 data 对象", text);
            check(cJSON_GetObjectItem(root, "id") == NULL,
                  "状态事件不应携带 id", text);
            cJSON_Delete(root);
            continue;
        }
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        const cJSON *ok = cJSON_GetObjectItem(root, "ok");
        check(cJSON_IsNumber(id), "应答帧缺数字 id 或类型错误", text);
        check(cJSON_IsBool(ok), "应答帧缺布尔 ok 或类型错误", text);
        if (cJSON_IsBool(ok)) {
            if (cJSON_IsTrue(ok)) {
                check(cJSON_GetObjectItem(root, "data") != NULL,
                      "ok:true 缺 data", text);
            } else {
                check(cJSON_IsString(cJSON_GetObjectItem(root, "err")),
                      "ok:false 缺字符串 err 或类型错误", text);
            }
        }
        cJSON_Delete(root);
    }
}

// 存活 cJSON 分配块数必须回到基线:根对象、未挂载子树、键/值字符串与
// 序列化缓冲都必须释放(不增长即无泄漏)。
static void check_json_balanced(size_t baseline, const char *where)
{
    size_t live = test_json_live_allocs();
    if (live != baseline) {
        char detail[96];
        snprintf(detail, sizeof(detail), "live=%zu baseline=%zu", live,
                 baseline);
        check(false, where, detail);
    }
}

// 驱动扫描结果必须平衡:结果被消费或丢弃,且没有被下一次扫描覆盖。
// 桩的统计是就地更新的静态结构,基线必须按值拷贝而不是保存指针。
typedef struct {
    unsigned allocs;
    unsigned frees;
} scan_base_t;

static scan_base_t scan_base_capture(void)
{
    const prov_scan_stats_t *st = test_prov_scan_stats();
    return (scan_base_t){ .allocs = st->allocs, .frees = st->frees };
}

static void check_scan_balanced(scan_base_t base, unsigned allocs_delta)
{
    const prov_scan_stats_t *st = test_prov_scan_stats();
    char detail[128];
    snprintf(detail, sizeof(detail),
             "allocs=%u frees=%u expect=base+%u live=%d overwritten=%u",
             st->allocs, st->frees, allocs_delta, (int)st->live,
             st->overwritten);
    check(st->live == false, "驱动扫描结果仍被持有", detail);
    check(st->allocs == st->frees, "驱动扫描结果分配/释放不平衡", detail);
    check(st->allocs == base.allocs + allocs_delta &&
              st->frees == base.frees + allocs_delta,
          "驱动扫描结果分配次数与预期不符", detail);
    check(st->overwritten == 0, "未释放结果被新扫描覆盖", detail);
}

// 按应答 id 取出唯一的完整应答文本(不存在返回 NULL)。同一 id 的完整
// 应答在窗口内必须唯一,否则说明重复发送。
static const char *reply_text(const tx_msg_t *msgs, size_t n, uint32_t gen,
                              const char *id_field)
{
    const char *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!msgs[i].complete || msgs[i].gen != gen) continue;
        if (strstr(msgs[i].text, id_field) == NULL) continue;
        assert(found == NULL);
        found = msgs[i].text;
    }
    return found;
}

// ---- 事件与消息驱动 ----

static struct ble_gatt_chr_def g_rx_chr;

// 连接/断开同时维护桩的连接表:提交按句柄解析到的是"当前连接"。
// 真实顺序:连接表插入先于 CONNECT 回调;断开先删表项再派发 DISCONNECT。
static unsigned raise_connect(uint16_t conn)
{
    unsigned serial = test_prov_conn_attach(conn);
    struct ble_gap_event ev = {0};
    ev.type = BLE_GAP_EVENT_CONNECT;
    ev.connect.status = 0;
    ev.connect.conn_handle = conn;
    assert(gap_event(&ev, NULL) == 0);
    return serial;
}

static void raise_disconnect(uint16_t conn, int reason)
{
    test_prov_conn_detach(conn);
    struct ble_gap_event ev = {0};
    ev.type = BLE_GAP_EVENT_DISCONNECT;
    ev.disconnect.reason = reason;
    ev.disconnect.conn.conn_handle = conn;
    assert(gap_event(&ev, NULL) == 0);
}

static void raise_subscribe(uint16_t conn, bool on)
{
    struct ble_gap_event ev = {0};
    ev.type = BLE_GAP_EVENT_SUBSCRIBE;
    ev.subscribe.conn_handle = conn;
    ev.subscribe.attr_handle = s_tx_val_handle;
    ev.subscribe.cur_notify = on ? 1 : 0;
    assert(gap_event(&ev, NULL) == 0);
}

static void raise_mtu(uint16_t conn, uint16_t mtu)
{
    test_prov_conn_set_mtu(conn, mtu);
    struct ble_gap_event ev = {0};
    ev.type = BLE_GAP_EVENT_MTU;
    ev.mtu.conn_handle = conn;
    ev.mtu.value = mtu;
    assert(gap_event(&ev, NULL) == 0);
}

static int try_deliver_frame(const uint8_t *frame, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
    struct ble_gatt_access_ctxt ctxt = {
        .op = BLE_GATT_ACCESS_OP_WRITE_CHR,
        .chr = &g_rx_chr,
        .om = om,
    };
    int rc = gatt_access(0, 0, &ctxt, NULL);
    os_mbuf_free_chain(om);
    return rc;
}

// 按给定单帧净荷切帧并经生产 GATT 写回调入队;返回 0 或队列背压错误。
static int deliver_message(const char *json, size_t payload)
{
    size_t total = strlen(json);
    size_t frames = suite_prov_frame_count(total, payload);
    uint8_t buf[SUITE_PROV_PAYLOAD_MAX + SUITE_PROV_HEADER_LEN];
    for (size_t i = 0; i < frames; i++) {
        size_t flen = suite_prov_frame_build((const uint8_t *)json, total,
                                             payload, i, buf, sizeof(buf));
        assert(flen > SUITE_PROV_HEADER_LEN);
        int rc = try_deliver_frame(buf, flen);
        if (rc != 0) return rc;
    }
    return 0;
}

static void rx_json(const char *json, size_t payload)
{
    assert(deliver_message(json, payload) == 0);
}

// 恢复 prov 任务处理完队列中的全部事件,再等待其停靠。
static void run_and_park(void)
{
    test_prov_resume_event();
    assert(test_prov_wait_park() == PARK_WAIT_MS);
}

static void hangup_and_connect(uint16_t conn)
{
    raise_disconnect(conn, 19);
    run_and_park();
    raise_connect(conn);
    run_and_park();
}

// ---- 并发交错:独立 host 线程 + 通知提交闸门 ----
//
// 真实的 NimBLE 顺序:host 线程先删除旧连接表项,再派发 DISCONNECT 回调,
// 之后才插入新表项并派发 CONNECT 回调;生产把这些回调与"终校验 + 提交"
// 放进同一串行化边界。这里的 host 线程按同一顺序执行,闸门只决定测试从
// 哪个确定点开始交错,不根据生产是否持锁调整调度:
//   - 正确实现:host 的 GAP 回调阻塞在发送线程持有的锁上,换机完成序号
//     必然大于提交通过闸门的序号;
//   - 锁失效:host 在闸门内就完成换机,序号关系反转,且旧帧会落到新连接。

#define GATE_WAIT_MS 2000UL        // 等待发送线程进入闸门
#define HOST_EARLY_WAIT_MS 1000UL  // 闸门内等待 host 完成(正确时应超时)
#define HOST_FINISH_WAIT_MS 5000UL // 放行后等待 host 完成

static mtx_t s_host_lock;
static cnd_t s_host_cv;
static bool s_host_done;
static uint32_t s_host_seq;

static int host_sequence_thread(void *arg)
{
    void (*seq)(void) = arg;
    seq();
    uint32_t seq_no = test_prov_seq_bump();
    mtx_lock(&s_host_lock);
    s_host_seq = seq_no;
    s_host_done = true;
    cnd_broadcast(&s_host_cv);
    mtx_unlock(&s_host_lock);
    return 0;
}

// 有界等待 host 线程完成;返回是否完成(超时返回 false,不挂死门禁)。
static bool wait_host_done(unsigned long timeout_ms)
{
    struct timespec deadline;
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    mtx_lock(&s_host_lock);
    int rc = thrd_success;
    while (!s_host_done && rc != thrd_timedout) {
        rc = cnd_timedwait(&s_host_cv, &s_host_lock, &deadline);
    }
    bool done = s_host_done;
    mtx_unlock(&s_host_lock);
    return done;
}

// 在发送线程的第 skip+1 次提交闸门内运行 host 换机序列。命令必须已
// 经 rx_json 入队且 prov 任务处于停靠状态。完成后 prov 任务停靠回
// 队列接收,可继续发送后续命令。
static void run_gated_switch(unsigned skip, void (*host_seq)(void))
{
    assert(!s_host_done);
    test_prov_arm_notify_gate(skip);
    test_prov_resume_event();  // 发送线程处理命令并停在闸门内
    assert(test_prov_wait_notify_gate(GATE_WAIT_MS));

    thrd_t host;
    assert(thrd_create(&host, host_sequence_thread, host_seq) == thrd_success);
    (void)wait_host_done(HOST_EARLY_WAIT_MS);  // 正确实现必然超时
    test_prov_release_notify_gate();
    assert(wait_host_done(HOST_FINISH_WAIT_MS));  // 放行后必须能完成
    assert(thrd_join(host, NULL) == thrd_success);

    // 串行化断言:换机完成必须晚于提交通过闸门(host 被生产锁挡住)。
    assert(s_host_seq > test_prov_gate_seq());

    assert(test_prov_wait_park() == PARK_WAIT_MS);

    // 复位 host 完成标志供下一个场景使用(顺序计数器保持全局单调)。
    mtx_lock(&s_host_lock);
    s_host_done = false;
    mtx_unlock(&s_host_lock);
}

// host 线程序列(同一 host 线程内按真实顺序执行)。

// 断连 + 同句柄重连(新连接默认 MTU 23,只建立自己的订阅)。
static void host_switch_default(void)
{
    raise_disconnect(0, 19);   // 表项删除 → DISCONNECT 回调(取生产锁)
    raise_connect(0);          // 表项插入 → CONNECT 回调(取生产锁)
    raise_subscribe(0, true);
}

// 断连 + 同句柄重连,新手机自行协商 MTU 247。
static void host_switch_mtu247(void)
{
    raise_disconnect(0, 19);
    raise_connect(0);
    raise_mtu(0, 247);
    raise_subscribe(0, true);
}

// host 异常复位:连接表清空 + reset 回调(取生产锁),随后新手机重连。
static void host_reset_then_reconnect(void)
{
    test_prov_conn_detach(0);  // host 复位清空连接表
    assert(ble_hs_cfg.reset_cb != NULL);
    ble_hs_cfg.reset_cb(5);    // 生产 link_on_reset(取生产锁)
    raise_connect(0);
    raise_subscribe(0, true);
}

// ---- 消息脚本 ----

static const char *k_hello_ok = "{\"id\":1,\"cmd\":\"hello\",\"pin\":\"135790\"}";
static const char *k_hello_bad = "{\"id\":1,\"cmd\":\"hello\",\"pin\":\"000000\"}";
static const char *k_status_cmd = "{\"id\":2,\"cmd\":\"status\"}";
static const char *k_scan_cmd = "{\"id\":3,\"cmd\":\"wifi_scan\"}";
static const char *k_provinces_cmd = "{\"id\":4,\"cmd\":\"provinces\"}";
static const char *k_cities_cmd = "{\"id\":5,\"cmd\":\"cities\",\"p\":0}";

// ---- 场景 1:阻塞扫描期间的断开与同句柄重连 ----

static void scan_hook_switch_phone(void)
{
    // 扫描阻塞期间:旧手机积压一个受保护命令(A 仍为当前链路)
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    // A 的迟到订阅与 MTU 事件也在此刻入队(晚于扫描命令、早于断开)
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    // A 断开、B 以同一句柄 0 重连并订阅(未 hello、未协商 MTU)
    raise_disconnect(0, 19);
    raise_connect(0);
    raise_subscribe(0, true);
}

static void scenario_scan_reconnect_isolation(void)
{
    // A:连接、订阅、协商 247、hello。
    raise_connect(0);
    run_and_park();
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    uint32_t gen_a = link_snapshot().gen;
    assert(link_snapshot().mtu == 247);

    size_t mark = g_frame_count;
    test_prov_set_scan_hook(scan_hook_switch_phone);
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MAX);  // A 发起扫描(阻塞期间切换)
    run_and_park();

    uint32_t gen_b = link_snapshot().gen;
    unsigned serial_b = test_prov_conn_serial(0);
    assert(serial_b != 0);
    assert(gen_b != gen_a);
    assert(link_snapshot().conn == 0);
    assert(link_snapshot().subscribed);  // B 自己的订阅
    assert(link_snapshot().mtu == SUITE_PROV_MTU_DEFAULT);  // 不继承 247

    // 旧会话受保护数据(扫描结果/积压 status)不得发给未 hello 的 B。
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_b, "\"nets\"") == 0);
    assert(!any_protected(msgs, n, gen_b));
    assert(!any_protected_conn(msgs, n, serial_b));
    assert(frames_to_conn(mark, serial_b) == 0);  // 含不完整片段
    // 积压的旧 status 请求也不得有任何应答(应答属于旧代次)。
    assert(count_messages(msgs, n, gen_a, "\"id\":2") == 0);

    // B 未 hello 前请求受保护命令:只得到 need_pin,而不是数据。
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_b, "\"need_pin\"") == 1);
    assert(!any_protected(msgs, n, gen_b));

    // B 正确 hello:恢复正常配网流程;未协商 MTU,帧长必须 ≤ 23-3。
    size_t mark2 = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_b, "\"ok\":true") == 1);
    assert(any_protected(msgs, n, gen_b));  // hello 后允许收到状态
    for (size_t i = mark2; i < g_frame_count; i++) {
        assert(g_frames[i].gen == gen_b);
        assert(g_frames[i].conn == serial_b);
        assert(g_frames[i].len <= 23 - 3);
    }

    // 恢复后的正常扫描:应答必须是 {"data":{"nets":[...]}} 契约,
    // 网页读取 data.nets(而不是把数组直接作为 data)。
    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "HomeNet");
    aps[0].rssi = -42;
    aps[0].authmode = 3;
    test_prov_set_scan_aps(aps, 1);
    size_t mark3 = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark3, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_b, "\"nets\":[") == 1);
    assert(count_messages(msgs, n, gen_b, "HomeNet") == 1);
    assert(count_messages(msgs, n, gen_b, "\"id\":3") == 1);
    assert(count_messages(msgs, n, gen_b, "\"ok\":true") == 1);
    for (size_t i = mark3; i < g_frame_count; i++) {
        assert(g_frames[i].len <= 23 - 3);  // B 未协商 MTU
    }
}

// ---- 场景 2:队列满丢弃 GAP 事件后的隔离与恢复 ----

static void scan_hook_flood_queue(void)
{
    // 8 个旧请求(每帧一个队列项)填满深度 8 的队列;随后的事件全部被
    // 丢弃——但 GAP 回调已更新链路代次/订阅/MTU 快照。
    const uint8_t junk[] = { SUITE_PROV_FLAG_SINGLE, 0x00, 'x' };
    for (int i = 0; i < PROV_QUEUE_DEPTH + 1; i++) {
        (void)try_deliver_frame(junk, sizeof(junk));
    }
    raise_disconnect(0, 19);
    raise_connect(0);
    raise_subscribe(0, true);
    raise_mtu(0, 247);
}

static void scenario_queue_full_recovery(void)
{
    // C:连接、订阅、协商 247、hello(单帧命令需要大 MTU)。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    uint32_t gen_c = link_snapshot().gen;

    size_t mark = g_frame_count;
    test_prov_set_scan_hook(scan_hook_flood_queue);
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();

    uint32_t gen_d = link_snapshot().gen;
    unsigned serial_d = test_prov_conn_serial(0);
    assert(serial_d != 0);
    assert(gen_d != gen_c);
    assert(link_snapshot().subscribed);  // 订阅事件被丢弃,状态仍来自回调快照
    assert(link_snapshot().mtu == 247);  // MTU 事件被丢弃,同样已恢复
    // 被丢弃的旧请求不得产生任何应答/数据(帧在队列满时被拒绝,已入队的
    // 8 帧在恢复后按代次丢弃)。
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(frames_with_gen(mark, gen_d) == 0);
    assert(frames_to_conn(mark, serial_d) == 0);
    assert(!any_protected(msgs, n, gen_c) && !any_protected(msgs, n, gen_d));

    // D 未 hello:不继承 C 的授权;坐标/SSID/IP 一帧都不给。
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_d, "\"need_pin\"") == 1);
    assert(!any_protected(msgs, n, gen_d));

    // D 正确 hello:恢复流程;帧长符合协商后的 247,而不是默认 23。
    size_t mark2 = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_d, "\"ok\":true") == 1);
    bool saw_big_frame = false;
    for (size_t i = mark2; i < g_frame_count; i++) {
        assert(g_frames[i].gen == gen_d);
        assert(g_frames[i].len <= 247 - 3);
        if (g_frames[i].len > 23 - 3) saw_big_frame = true;
    }
    assert(saw_big_frame);  // 状态推送在 247 下多帧,证明快照 MTU 已恢复
}

// ---- 场景 3:MTU 23/247 与订阅不跨连接继承 ----

static void scenario_mtu_isolation_and_framing(void)
{
    // E:默认 23 连接、订阅、hello;provinces 应答必须按 23 切帧。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();

    size_t mark = g_frame_count;
    rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(n == 1 && msgs[0].complete);
    size_t len23 = strlen(msgs[0].text);
    assert(msgs[0].frames == suite_prov_frame_count(len23, SUITE_PROV_PAYLOAD_MIN));
    assert(msgs[0].frames > 1);
    for (size_t i = mark; i < g_frame_count; i++) {
        assert(g_frames[i].len <= 23 - 3);
    }

    // E 协商 247 后同一命令切帧数减少,帧长上限 247-3。
    raise_mtu(0, 247);
    run_and_park();
    size_t mark2 = g_frame_count;
    rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(n == 1 && msgs[0].complete);
    size_t len247 = strlen(msgs[0].text);
    assert(len247 == len23);
    assert(msgs[0].frames == suite_prov_frame_count(len247, SUITE_PROV_PAYLOAD_MAX));
    assert(msgs[0].frames < suite_prov_frame_count(len247, SUITE_PROV_PAYLOAD_MIN));
    for (size_t i = mark2; i < g_frame_count; i++) {
        assert(g_frames[i].len <= 247 - 3);
    }

    // F 同句柄重连:订阅与 MTU 都不继承,重新 hello 前订阅为未被采纳的
    // 默认状态;协商前帧长回到 23 上限。
    hangup_and_connect(0);
    assert(link_snapshot().mtu == SUITE_PROV_MTU_DEFAULT);
    assert(!link_snapshot().subscribed);
    raise_subscribe(0, true);
    run_and_park();
    assert(link_snapshot().mtu == SUITE_PROV_MTU_DEFAULT);
    size_t mark3 = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark3, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "\"ok\":true") == 1);
    for (size_t i = mark3; i < g_frame_count; i++) {
        assert(g_frames[i].len <= 23 - 3);
    }
}

// ---- 场景 4:逐帧发送中与提交前的链路切换 ----
//
// 契约的另一半(与"构造失败不得发送"互补,成对验收):
//   - 构造失败发生在 tx_json 之前:必须零帧(含片段),见
//     envelope_double_failure_case 与 scan_total_json_failure;
//   - 发送已经开始的中途失败:允许对端只看到不完整消息,但剩余帧不得
//     交给新连接、挂起的消息不得被当作完整应答重发。
static void precommit_switch(void)
{
    // mbuf 已分配、终校验之前(尚未取发送锁):链路切换。
    raise_disconnect(0, 19);
    raise_connect(0);
    raise_subscribe(0, true);
}

static void scenario_mid_send_and_precommit_switch(void)
{
    // G:默认 23、订阅、hello;cities 应答在 23 下多帧。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    uint32_t gen_g = link_snapshot().gen;
    unsigned serial_g = test_prov_conn_serial(0);
    assert(serial_g != 0 && link_snapshot().mtu == SUITE_PROV_MTU_DEFAULT);

    // 第 1 帧正常交付旧手机;第 2 帧在提交闸门处与 host 换机交错:
    // 旧表项已删除、新表项尚未插入,提交按句柄找不到连接,剩余帧全部
    // 放弃;新手机零帧,消息不完整。
    size_t mark = g_frame_count;
    rx_json(k_cities_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_gated_switch(1, host_switch_default);

    uint32_t gen_h = link_snapshot().gen;
    unsigned serial_h = test_prov_conn_serial(0);
    assert(serial_h != 0 && serial_h != serial_g);
    assert(gen_h != gen_g);

    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(n == 1 && !msgs[0].complete && msgs[0].gen == gen_g);
    assert(msgs[0].conn == serial_g);
    assert(msgs[0].frames == 1);
    // 实际交付的第 1 帧:START 帧、按实际连接 MTU 切帧、内容是对应应答。
    assert(frames_to_conn(mark, serial_g) == 1);
    assert(g_frames[mark].att_mtu == SUITE_PROV_MTU_DEFAULT);
    assert(g_frames[mark].len <= SUITE_PROV_MTU_DEFAULT - 3);
    assert(g_frames[mark].data[0] == SUITE_PROV_FLAG_START);
    assert(memcmp(g_frames[mark].data + SUITE_PROV_HEADER_LEN, "{\"id\":5",
                  7) == 0);
    // 新手机零帧(含不完整片段),也没有完整的旧应答。
    assert(frames_to_conn(mark, serial_h) == 0);
    assert(!any_protected_conn(msgs, n, serial_h));
    assert(count_messages(msgs, n, gen_g, "\"ok\":true") == 0);

    // H 正确 hello 后恢复正常流程。
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    assert(link_snapshot().gen == gen_h);

    // 终校验之前(分配 mbuf 时)换机:整个应答零帧(首帧也未提交)。
    size_t mark2 = g_frame_count;
    test_prov_set_precommit_hook(precommit_switch);
    rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();

    uint32_t gen_i = link_snapshot().gen;
    assert(gen_i != gen_h);
    assert(frames_with_gen(mark2, gen_h) == 0);
    assert(frames_with_gen(mark2, gen_i) == 0);
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(n == 0);

    // I 正确 hello:流程仍然可用。
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_i, "\"ok\":true") == 1);
}

// ---- 场景 5:错误配对码计数与锁定断开 ----

static void scenario_wrong_pin_and_recovery(void)
{
    // J:连接、订阅;连续 3 次错误配对码 → bad_pin 应答 + 主动断开。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    size_t mark = g_frame_count;
    for (int i = 0; i < 3; i++) {
        rx_json(k_hello_bad, SUITE_PROV_PAYLOAD_MIN);
        run_and_park();
    }
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    uint32_t gen_j = link_snapshot().gen;  // 仍为 J(终止事件由栈随后上报)
    assert(count_messages(msgs, n, gen_j, "\"bad_pin\"") == 3);
    assert(count_messages(msgs, n, gen_j, "\"ok\":true") == 0);
    assert(test_prov_terminate_calls() == 1);

    // 栈执行终止后断开;K 重连:失败计数随连接重置,正确 hello 可用。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    size_t mark2 = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    uint32_t gen_k = link_snapshot().gen;
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen_k, "\"ok\":true") == 1);
    assert(any_protected(msgs, n, gen_k));  // hello 后状态推送恢复
}

// ---- 场景 6:终校验之后、通知提交之前的换机(独立 host 线程交错) ----

// 发送线程进入提交闸门(终校验已通过,按句柄交付尚未发生)后,host 线程
// 执行换机序列。正确实现下 host 的 GAP 回调阻塞在发送线程持有的锁上:
//   表项删除后提交按句柄找不到连接,旧受保护帧被丢弃,新连接零帧;
// 锁失效时换机在提交前生效,帧会落到新连接,下列断言直接失败。
static void gated_switch_case(void (*host_seq)(void), unsigned b_mtu)
{
    // A:连接、订阅、协商 247、hello(受保护应答在 247 切帧,多帧)。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    unsigned serial_a = test_prov_conn_serial(0);
    uint32_t gen_a = link_snapshot().gen;
    assert(serial_a != 0);
    assert(link_snapshot().mtu == 247);

    // A 请求受保护状态:提交第一帧时与 host 换机交错。
    size_t mark = g_frame_count;
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MAX);
    run_gated_switch(0, host_seq);

    unsigned serial_b = test_prov_conn_serial(0);
    uint32_t gen_b = link_snapshot().gen;
    assert(serial_b != 0 && serial_b != serial_a);
    assert(gen_b != gen_a);
    assert(link_snapshot().conn == 0);
    assert(link_snapshot().subscribed);  // B 自己的订阅
    assert(link_snapshot().mtu == b_mtu);  // B 自己的 MTU,不继承 A 的 247

    // B 收到零帧旧会话数据 —— 按连接序号统计,含不完整片段。
    assert(frames_to_conn(mark, serial_b) == 0);
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(!any_protected_conn(msgs, n, serial_b));
    assert(count_messages_conn(msgs, n, serial_b, "\"ok\":true") == 0);
    // 窗口内实际交付的帧(如有)必须仍属于旧连接与旧代次,且满足旧连接
    // 协商的 MTU;受保护应答没有任何完整版本泄漏到任何连接。
    for (size_t i = mark; i < g_frame_count; i++) {
        assert(g_frames[i].conn == serial_a);
        assert(g_frames[i].gen == gen_a);
        assert(g_frames[i].att_mtu == 247);
        assert(g_frames[i].len <= 247 - 3);
    }
    assert(count_messages_conn(msgs, n, serial_a, "\"ok\":true") == 0);

    // B 未 hello:受保护命令只得到 need_pin,而不是数据。
    size_t mark2 = g_frame_count;
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages_conn(msgs, n, serial_b, "\"need_pin\"") == 1);
    assert(!any_protected_conn(msgs, n, serial_b));

    // B 正确 hello:状态推送、扫描与省市命令恢复正常;帧长符合 B 的 MTU。
    size_t mark3 = g_frame_count;
    rx_json(k_hello_ok, b_mtu == 247 ? SUITE_PROV_PAYLOAD_MAX
                                     : SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark3, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages_conn(msgs, n, serial_b, "\"ok\":true") == 1);
    assert(any_protected_conn(msgs, n, serial_b));
    bool saw_big_frame = false;
    for (size_t i = mark3; i < g_frame_count; i++) {
        assert(g_frames[i].conn == serial_b);
        assert(g_frames[i].gen == gen_b);
        assert(g_frames[i].len <= b_mtu - 3);
        if (g_frames[i].len > 23 - 3) saw_big_frame = true;
    }
    assert(saw_big_frame == (b_mtu == 247));

    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "HomeNet");
    aps[0].rssi = -42;
    aps[0].authmode = 3;
    test_prov_set_scan_aps(aps, 1);
    size_t mark4 = g_frame_count;
    rx_json(k_scan_cmd, b_mtu == 247 ? SUITE_PROV_PAYLOAD_MAX
                                     : SUITE_PROV_PAYLOAD_MIN);
    rx_json(k_provinces_cmd, b_mtu == 247 ? SUITE_PROV_PAYLOAD_MAX
                                          : SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark4, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages_conn(msgs, n, serial_b, "HomeNet") == 1);
    assert(count_messages_conn(msgs, n, serial_b, "\"nets\":[") == 1);
    assert(count_messages_conn(msgs, n, serial_b, "\"i\":0") == 1);
    for (size_t i = mark4; i < g_frame_count; i++) {
        assert(g_frames[i].conn == serial_b);
        assert(g_frames[i].len <= b_mtu - 3);
    }
    // 扫描结果已被消费:桩不得记录"被新扫描覆盖"。
    assert(test_prov_scan_stats()->live == false);
    assert(test_prov_scan_stats()->overwritten == 0);
}

static void scenario_notify_commit_switch_isolation(void)
{
    gated_switch_case(host_switch_default, SUITE_PROV_MTU_DEFAULT);  // B 默认 23
    gated_switch_case(host_switch_mtu247, 247);                      // B 协商 247
    // reset 与发送交错:host 复位后新手机立即重连,同样不得吞掉旧帧。
    gated_switch_case(host_reset_then_reconnect, SUITE_PROV_MTU_DEFAULT);
}

// ---- 场景 7:扫描结果的驱动所有权(P2 回归) ----

// 正常扫描:get 消费结果并释放;应答仍是 data.nets 契约。
static void scan_normal_consume(void)
{
    wifi_ap_record_t aps[3] = { 0 };
    strcpy((char *)aps[0].ssid, "Alpha");
    aps[0].rssi = -40;
    aps[0].authmode = 3;
    strcpy((char *)aps[1].ssid, "Beta");
    aps[1].rssi = -70;
    aps[1].authmode = 0;
    strcpy((char *)aps[2].ssid, "Alpha");  // 同名去重取最强
    aps[2].rssi = -55;
    aps[2].authmode = 3;
    test_prov_set_scan_aps(aps, 3);

    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "\"nets\":[") == 1);
    assert(count_messages(msgs, n, link_snapshot().gen, "Alpha") == 1);
    assert(count_messages(msgs, n, link_snapshot().gen, "Beta") == 1);
    // 去重取最强(-40)且按信号从强到弱排序:Alpha 在 Beta 之前。
    size_t alpha_at = 0, beta_at = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = strstr(msgs[i].text, "\"nets\":[");
        if (!p) continue;
        const char *a = strstr(p, "Alpha");
        const char *b = strstr(p, "Beta");
        assert(a && b && a < b);
        alpha_at = (size_t)(a - p);
        beta_at = (size_t)(b - p);
    }
    assert(alpha_at < beta_at);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->live == false);        // 驱动结果已释放
    assert(st->allocs == st->frees);  // 无遗留
    assert(st->overwritten == 0);
}

// 正常非空扫描的内容契约:隐藏 SSID 过滤、同名去重取最强、按信号排序、
// 数量上限 20、sec 字段与 data.nets 结构。
static void scan_normal_filters_and_cap(void)
{
    wifi_ap_record_t aps[28] = { 0 };
    unsigned n = 0;
    for (unsigned i = 0; i < 25; i++) {
        snprintf((char *)aps[n].ssid, sizeof(aps[n].ssid), "AP%02u", i);
        aps[n].rssi = (int8_t)(-30 - (int)i);
        aps[n].authmode = (i % 4);
        n++;
    }
    aps[n].ssid[0] = '\0';  // 隐藏 SSID:即使信号最强也不得泄漏
    aps[n].rssi = -10;
    aps[n].authmode = 3;
    n++;
    strcpy((char *)aps[n].ssid, "AP00");  // 同名更弱:不得覆盖 -30
    aps[n].rssi = -90;
    aps[n].authmode = 0;
    n++;
    strcpy((char *)aps[n].ssid, "AP01");  // 同名更强:覆盖 -31 并升到首位
    aps[n].rssi = -5;
    aps[n].authmode = 3;
    n++;
    test_prov_set_scan_aps(aps, n);

    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t total = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    const char *text = NULL;
    for (size_t i = 0; i < total; i++) {
        if (strstr(msgs[i].text, "\"nets\":[")) text = msgs[i].text;
    }
    assert(text != NULL);
    assert(strstr(text, "\"ok\":true") != NULL);
    assert(strstr(text, "\"id\":3") != NULL);
    // 隐藏 SSID 不出现。
    assert(strstr(text, "\"ssid\":\"\"") == NULL);
    // 25 个唯一网络,应答条目数上限 20。
    unsigned items = 0;
    for (const char *q = text; (q = strstr(q, "\"ssid\":")) != NULL; q += 7) {
        items++;
    }
    assert(items == 20);
    // 去重取最强 + 从强到弱排序:AP01(-5)在 AP00(-30)之前;
    // AP00 未被 -90 的同名项覆盖;sec 使用 WIFI_AUTH_* 枚举值。
    const char *ap01 = strstr(text, "\"AP01\"");
    const char *ap00 = strstr(text, "\"AP00\"");
    assert(ap01 && ap00 && ap01 < ap00);
    assert(strstr(text, "\"AP01\",\"rssi\":-5,\"sec\":3") != NULL);
    assert(strstr(text, "\"AP00\",\"rssi\":-30") != NULL);
    // 只保留最强的 20 个:AP19 在,AP20 起被截掉。
    assert(strstr(text, "\"AP19\"") != NULL);
    assert(strstr(text, "\"AP20\"") == NULL);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

// 空结果:同样必须消费/释放,应答为空 nets 列表(网页契约)。
static void scan_empty_result(void)
{
    test_prov_set_scan_aps(NULL, 0);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "\"nets\":[]") == 1);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

// 启动失败:没有驱动结果,应答 scan_busy,不应触碰所有权计数。
static void scan_start_failure(void)
{
    const prov_scan_stats_t *before = test_prov_scan_stats();
    unsigned allocs0 = before->allocs;
    unsigned frees0 = before->frees;
    test_prov_set_scan_start_fail(true);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "scan_busy") == 1);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 && st->frees == frees0);
    assert(st->live == false);
    assert(st->overwritten == 0);
}

// 扫描成功后换机:结果不属于新会话,必须释放且不得发送。
static void scan_switch_hook_abandon(void)
{
    raise_disconnect(0, 19);
    raise_connect(0);
    raise_subscribe(0, true);
}

static void scan_abandoned_on_switch(void)
{
    // 新连接已认证,先确认基线状态。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    unsigned serial_old = test_prov_conn_serial(0);

    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "ShouldNotLeak");
    aps[0].rssi = -30;
    aps[0].authmode = 3;
    test_prov_set_scan_aps(aps, 1);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;

    size_t mark = g_frame_count;
    test_prov_set_scan_hook(scan_switch_hook_abandon);
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();

    unsigned serial_new = test_prov_conn_serial(0);
    assert(serial_new != 0 && serial_new != serial_old);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 + 1);
    assert(st->live == false);        // 被放弃的结果已释放
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);     // 下一次扫描不会覆盖未释放结果
    assert(frames_to_conn(mark, serial_new) == 0);  // 新连接零帧
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(!any_protected_conn(msgs, n, serial_new));
    assert(count_messages_conn(msgs, n, serial_new, "ShouldNotLeak") == 0);

    // 恢复:新连接 hello 后正常扫描仍然可用(下一次扫描不得覆盖遗留结果)。
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_prov_set_scan_aps(aps, 1);
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    st = test_prov_scan_stats();
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

// 应用侧分配失败:必须丢弃驱动结果,应答 no_mem。
static void scan_app_alloc_failure(void)
{
    test_prov_fail_calloc_at(1);  // 让 cmd_wifi_scan 的 aps 分配失败
    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "NeverUsed");
    aps[0].rssi = -50;
    aps[0].authmode = 0;
    test_prov_set_scan_aps(aps, 1);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "no_mem") == 1);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 + 1);
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

// 第二个应用侧分配失败(去重表 ents),非空扫描:必须进入错误路径,
// 绝不能伪装成"没有网络"的成功空列表;结果与两端内存全部释放。
static void scan_ents_alloc_failure(void)
{
    test_prov_fail_calloc_at(2);  // aps 成功,ents 失败
    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "HalfBuilt");
    aps[0].rssi = -44;
    aps[0].authmode = 3;
    test_prov_set_scan_aps(aps, 1);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    uint32_t gen = link_snapshot().gen;
    assert(count_messages(msgs, n, gen, "no_mem") == 1);
    assert(count_messages(msgs, n, gen, "\"ok\":true") == 0);
    // 关键回归:非空扫描 + ents 失败绝不是 ok:true 的空列表。
    assert(count_messages(msgs, n, gen, "\"nets\"") == 0);
    assert(count_messages(msgs, n, gen, "HalfBuilt") == 0);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 + 1);  // 驱动结果已被消费
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

// 关键 JSON 分配/添加失败:任何一处失败都必须返回 no_mem 错误,而不是
// 部分/缺字段的成功结果;驱动结果与应用侧内存全部释放,会话保持可用。
static void scan_json_failure_case(int op, unsigned nth)
{
    static const wifi_ap_record_t one = { .ssid = "JsonFail",
                                          .rssi = -47, .authmode = 3 };
    test_prov_set_scan_aps(&one, 1);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;
    test_fail_json_at(op, nth);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_json_at(JSON_OP_NONE, 0);
    uint32_t gen = link_snapshot().gen;
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, gen, "no_mem") == 1);
    assert(count_messages(msgs, n, gen, "\"ok\":true") == 0);
    assert(count_messages(msgs, n, gen, "\"nets\"") == 0);
    assert(count_messages(msgs, n, gen, "JsonFail") == 0);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 + 1);
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

static void scan_json_failures(void)
{
    // nets 数组、结果项对象、ssid/rssi/sec 字段、数组追加、data 对象
    // 与 data.nets 追加,逐一注入失败。
    scan_json_failure_case(JSON_OP_NETS_ARRAY, 1);
    scan_json_failure_case(JSON_OP_CREATE_OBJECT, 1);    // 结果项
    scan_json_failure_case(JSON_OP_SSID, 1);
    scan_json_failure_case(JSON_OP_ADD_NUMBER, 1);       // rssi
    scan_json_failure_case(JSON_OP_ADD_NUMBER, 2);       // sec
    scan_json_failure_case(JSON_OP_ADD_ITEM_ARRAY, 1);
    scan_json_failure_case(JSON_OP_CREATE_OBJECT, 2);    // data 对象
    scan_json_failure_case(JSON_OP_ADD_ITEM_OBJECT, 1);  // data.nets
}

// 连错误应答也无法组装:不发送任何成功或部分结果(允许什么都不发),
// 资源仍必须释放;故障解除后恢复。
static void scan_total_json_failure(void)
{
    static const wifi_ap_record_t one = { .ssid = "AllFail", .rssi = -41,
                                          .authmode = 3 };
    test_prov_set_scan_aps(&one, 1);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;
    test_fail_all_json(true);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_all_json(false);

    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(n == 0);  // 不出现成功假象;错误帧也发不出去时可静默
    assert(g_frame_count == mark);  // 构造失败不得发送任何字节(含片段)
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->allocs == allocs0 + 1);
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);

    // 故障解除:正常扫描恢复,内容契约保持。
    size_t mark2 = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    n = collect_messages(mark2, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "AllFail") == 1);
    assert(count_messages(msgs, n, link_snapshot().gen, "\"nets\":[") == 1);
}

// 获取记录失败:不能伪装成功;驱动结果必须被 clear 释放。
static void scan_get_failure(void)
{
    wifi_ap_record_t aps[1] = { 0 };
    strcpy((char *)aps[0].ssid, "FetchFail");
    aps[0].rssi = -60;
    aps[0].authmode = 0;
    test_prov_set_scan_aps(aps, 1);
    test_prov_set_scan_get_fail(true);
    const prov_scan_stats_t *st0 = test_prov_scan_stats();
    unsigned allocs0 = st0->allocs;
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages(msgs, n, link_snapshot().gen, "\"ok\":true") == 0);
    assert(count_messages(msgs, n, link_snapshot().gen, "FetchFail") == 0);
    assert(count_messages(msgs, n, link_snapshot().gen, "scan_failed") == 1);
    const prov_scan_stats_t *st = test_prov_scan_stats();
    assert(st->get_fail == 1);
    assert(st->allocs == allocs0 + 1);
    assert(st->live == false);
    assert(st->allocs == st->frees);
    assert(st->overwritten == 0);
}

static void scenario_scan_result_ownership(void)
{
    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();

    scan_normal_consume();
    scan_normal_filters_and_cap();
    scan_empty_result();
    scan_start_failure();
    scan_abandoned_on_switch();
    scan_app_alloc_failure();
    scan_ents_alloc_failure();
    scan_json_failures();
    scan_total_json_failure();
    scan_get_failure();
}

// ---- 场景 7b:应答外层封装的失败处理(P2 回归) ----
//
// 每条用例都先让扫描结果完整构造成功(1 个 AP,data.nets 已就绪),再
// 破坏外层封装的一个环节:
//   - root 创建失败(JSON_OP_CREATE_OBJECT 第 3 次);
//   - id 字段分配/添加失败(JSON_OP_ADD_NUMBER 第 3 次);
//   - ok 字段分配/添加失败(JSON_OP_ADD_BOOL 第 1 次);
//   - root.data 键分配失败(JSON_OP_ADD_ITEM_OBJECT 第 2 次)——本轮 P2:
//     旧实现忽略返回值,继续发送 {"id":3,"ok":true} 并泄漏整个 data 子树;
//   - 序列化失败(JSON_OP_PRINT 第 1 次)。
// 断言:没有残缺或虚假的成功帧;能构造错误帧时收到完整 no_mem;错误帧
// 自身也失败时允许不发送;两条路径都不遗留 cJSON 分配与驱动扫描结果。

// 单点失败:成功应答组装失败后,错误应答必须能正常发送。
static void envelope_case(const char *name, int op, unsigned nth)
{
    static const wifi_ap_record_t one = { .ssid = "EnvelopeAp",
                                          .rssi = -45, .authmode = 3 };
    test_prov_set_scan_aps(&one, 1);
    scan_base_t base = scan_base_capture();
    size_t live_base = test_json_live_allocs();

    test_fail_json_at(op, nth);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_json_at(JSON_OP_NONE, 0);

    uint32_t gen = link_snapshot().gen;
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"ok\":true") == 0, name,
          "出现成功帧(虚假成功)");
    check(count_messages(msgs, n, gen, "\"nets\"") == 0, name,
          "出现部分结果");
    check(count_messages(msgs, n, gen, "EnvelopeAp") == 0, name,
          "泄漏扫描结果");
    check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 1, name,
          "缺少完整 no_mem 错误应答");
    check_scan_balanced(base, 1);
    check_json_balanced(live_base, name);
}

// 组合失败:成功应答组装失败,错误应答自身也构造/序列化失败。
// 契约区分:
//   - 构造失败不得发送:任何一帧都不允许(含不完整片段),因为失败发生在
//     tx_json 之前;唯一可能合法出现的窗口内容是完整异步状态事件
//     (ev:status,无 id;本请求不产生状态推送,故这里精确要求零帧);
//   - "传输中途失败允许产生不完整消息"是另一条契约,由
//     scenario_mid_send_and_precommit_switch 单独验证(挂起的消息必须
//     标为不完整,不得作为完整应答交付)。
// 资源侧:资源必须全部释放、驱动结果必须平衡。
static void envelope_double_failure_case(const char *name, int op, unsigned nth,
                                         int op2, unsigned nth2)
{
    static const wifi_ap_record_t one = { .ssid = "DoubleFail",
                                          .rssi = -49, .authmode = 3 };
    test_prov_set_scan_aps(&one, 1);
    scan_base_t base = scan_base_capture();
    size_t live_base = test_json_live_allocs();

    test_fail_json_at(op, nth);
    test_fail_json_more(op2, nth2);
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_json_at(JSON_OP_NONE, 0);

    uint32_t gen = link_snapshot().gen;
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"ok\":true") == 0, name,
          "出现成功帧(虚假成功)");
    check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 0, name,
          "错误帧发不出去时不应有其它应答");
    check(count_messages(msgs, n, gen, "DoubleFail") == 0, name,
          "泄漏扫描结果");
    check(count_messages(msgs, n, gen, "\"id\":") == 0, name,
          "构造失败仍出现应答帧");
    if (g_frame_count != mark) {
        char detail[64];
        snprintf(detail, sizeof(detail), "frames=%zu", g_frame_count - mark);
        check(false, name, detail);
    }
    check_scan_balanced(base, 1);
    check_json_balanced(live_base, name);
}

// 多次失败交替注入 + 故障解除:存活分配块数不得增长,恢复后的下一次
// 请求必须完整成功(内容与结构都保持契约)。
static void scan_envelope_repeat_and_recover(void)
{
    static const wifi_ap_record_t one = { .ssid = "RepeatNet",
                                          .rssi = -37, .authmode = 3 };
    static const int ops[] = { JSON_OP_CREATE_OBJECT, JSON_OP_ADD_ITEM_OBJECT,
                              JSON_OP_PRINT };
    static const unsigned nths[] = { 3, 2, 1 };  // 外层 root / root.data / 序列化
    test_prov_set_scan_aps(&one, 1);
    scan_base_t base = scan_base_capture();
    size_t live_base = test_json_live_allocs();

    for (unsigned round = 0; round < 9; round++) {
        int op = ops[round % 3];
        unsigned nth = nths[round % 3];
        test_fail_json_at(op, nth);
        size_t mark = g_frame_count;
        rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
        run_and_park();
        test_fail_json_at(JSON_OP_NONE, 0);

        uint32_t gen = link_snapshot().gen;
        tx_msg_t msgs[MAX_TX_MSGS];
        size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
        check_reply_frames(msgs, n, gen);
        check(count_messages(msgs, n, gen, "\"ok\":true") == 0,
              "重复失败:出现成功帧", NULL);
        check(count_messages(msgs, n, gen, "RepeatNet") == 0,
              "重复失败:泄漏扫描结果", NULL);
        check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 1,
              "重复失败:缺少完整 no_mem 应答", NULL);
    }
    check_scan_balanced(base, 9);
    check_json_balanced(live_base, "重复失败后存活分配");

    // 故障解除:下一次请求完整成功,内容契约保持。
    size_t mark = g_frame_count;
    rx_json(k_scan_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    uint32_t gen = link_snapshot().gen;
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"ok\":true") == 1,
          "恢复后缺少成功帧", NULL);
    check(count_messages(msgs, n, gen, "\"nets\":[") == 1,
          "恢复后缺少 data.nets 结构", NULL);
    check(count_messages(msgs, n, gen, "RepeatNet") == 1,
          "恢复后缺少扫描结果内容", NULL);
    check_scan_balanced(base, 10);
    check_json_balanced(live_base, "恢复后存活分配");
}

// 其它成功应答的契约:合法空数据 / 必需字段 / 数据构造失败的区分。
static void reply_contract_cases(void)
{
    size_t live_base = test_json_live_allocs();
    tx_msg_t msgs[MAX_TX_MSGS];

    // hello 成功:data 字段必须存在(合法空对象),不能因为"没有内容"
    // 就省略 data——那与"数据构造失败"无法区分。
    size_t mark = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    uint32_t gen = link_snapshot().gen;
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"data\":{}") == 1,
          "hello 成功帧缺空 data 对象", NULL);
    check(count_messages(msgs, n, gen, "\"err\"") == 0, "hello 成功帧含错误",
          NULL);
    check_json_balanced(live_base, "hello 正常应答后存活分配");

    // status 应答:必需字段齐全(值来自测试桩)。
    mark = g_frame_count;
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    gen = link_snapshot().gen;
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    const char *reply = reply_text(msgs, n, gen, "\"id\":2");
    check(reply != NULL, "status 应答缺失", NULL);
    if (reply) {
        check(strstr(reply, "\"ok\":true") != NULL, "status 不是成功应答",
              reply);
        check(strstr(reply, "\"data\":{") != NULL, "status 缺 data", reply);
        check(strstr(reply, "\"wifi\":\"online\"") != NULL, "status 缺 wifi",
              reply);
        check(strstr(reply, "\"ip\":\"192.168.1.23\"") != NULL, "status 缺 ip",
              reply);
        check(strstr(reply, "\"time_synced\":true") != NULL,
              "status 缺 time_synced", reply);
        check(strstr(reply, "\"city\":{\"n\":\"Beijing\"") != NULL,
              "status 缺 city", reply);
    }
    check_json_balanced(live_base, "status 正常应答后存活分配");

    // status 数据构造失败(第一个必需字段 wifi 添加失败):不得发送缺
    // 字段的成功帧,回完整 no_mem。
    test_fail_json_at(JSON_OP_SSID, 1);
    mark = g_frame_count;
    rx_json(k_status_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_json_at(JSON_OP_NONE, 0);
    gen = link_snapshot().gen;
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"ok\":true") == 0,
          "status 数据构造失败仍报成功", NULL);
    check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 1,
          "status 数据构造失败缺 no_mem", NULL);
    check_json_balanced(live_base, "status 数据构造失败后存活分配");

    // hello 的 data 对象创建失败:同样不得发出缺 data 的成功帧。
    test_fail_json_at(JSON_OP_CREATE_OBJECT, 1);
    mark = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    test_fail_json_at(JSON_OP_NONE, 0);
    gen = link_snapshot().gen;
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    check(count_messages(msgs, n, gen, "\"ok\":true") == 0,
          "hello data 构造失败仍报成功", NULL);
    check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 1,
          "hello data 构造失败缺 no_mem", NULL);
    check_json_balanced(live_base, "hello data 构造失败后存活分配");

    // 正常 provinces:三个省份齐全。
    mark = g_frame_count;
    rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();
    gen = link_snapshot().gen;
    n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    check_reply_frames(msgs, n, gen);
    reply = reply_text(msgs, n, gen, "\"id\":4");
    check(reply != NULL, "provinces 应答缺失", NULL);
    if (reply) {
        check(strstr(reply, "\"ok\":true") != NULL, "provinces 不是成功应答",
              reply);
        check(strstr(reply, "ProvinceAlphaLong") != NULL,
              "provinces 缺第 1 项", reply);
        check(strstr(reply, "ProvinceBetaLong") != NULL, "provinces 缺第 2 项",
              reply);
        check(strstr(reply, "ProvinceGammaLong") != NULL,
              "provinces 缺第 3 项", reply);
    }
    check_json_balanced(live_base, "provinces 正常应答后存活分配");

    // provinces 组装失败(第 1 项的 n 字段失败 / 数组本身失败 / 条目
    // 追加失败 / 成功应答 root 创建失败):不得发送少条目的列表。
    static const struct {
        const char *name;
        int op;
        unsigned nth;
    } provinces_cases[] = {
        { "provinces n 字段失败", JSON_OP_SSID, 1 },
        { "provinces 数组分配失败", JSON_OP_NETS_ARRAY, 1 },
        { "provinces 条目追加失败", JSON_OP_ADD_ITEM_ARRAY, 1 },
        { "provinces 应答 root 失败", JSON_OP_CREATE_OBJECT, 4 },
    };
    for (size_t i = 0;
         i < sizeof(provinces_cases) / sizeof(provinces_cases[0]); i++) {
        test_fail_json_at(provinces_cases[i].op, provinces_cases[i].nth);
        mark = g_frame_count;
        rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MIN);
        run_and_park();
        test_fail_json_at(JSON_OP_NONE, 0);
        gen = link_snapshot().gen;
        n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
        check_reply_frames(msgs, n, gen);
        check(count_messages(msgs, n, gen, "\"ok\":true") == 0,
              provinces_cases[i].name, "出现成功帧(部分列表)");
        check(count_messages(msgs, n, gen, "Province") == 0,
              provinces_cases[i].name, "出现部分省市数据");
        check(count_messages(msgs, n, gen, "\"err\":\"no_mem\"") == 1,
              provinces_cases[i].name, "缺少完整 no_mem 错误应答");
        check_json_balanced(live_base, provinces_cases[i].name);
    }
}

// 场景入口:先建立已授权会话,再跑全部应答封装用例。
static void scenario_reply_envelope_failures(void)
{
    // 记账基线:此前所有场景(含各类注入)都不得留下 cJSON 分配。
    check_json_balanced(0, "进入应答封装场景前已有存活分配");

    hangup_and_connect(0);
    raise_subscribe(0, true);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MIN);
    run_and_park();

    // P2 本体:root.data 键分配失败不得再发送 {"id":3,"ok":true}。
    envelope_case("root.data 挂载失败", JSON_OP_ADD_ITEM_OBJECT, 2);
    // 外层 root 创建失败(结果子树必须由调用方释放)。
    envelope_case("外层 root 创建失败", JSON_OP_CREATE_OBJECT, 3);
    // id / ok 必需字段的分配或键分配失败。
    envelope_case("id 字段失败", JSON_OP_ADD_NUMBER, 3);
    envelope_case("ok 字段失败", JSON_OP_ADD_BOOL, 1);
    // 序列化失败。
    envelope_case("序列化失败", JSON_OP_PRINT, 1);
    // 成功应答失败后,错误应答自身也失败:允许不发送,资源必须释放。
    envelope_double_failure_case("错误应答 root 失败", JSON_OP_ADD_ITEM_OBJECT,
                                 2, JSON_OP_CREATE_OBJECT, 4);
    envelope_double_failure_case("错误应答 id 失败", JSON_OP_ADD_ITEM_OBJECT, 2,
                                 JSON_OP_ADD_NUMBER, 4);
    // 错误应答 ok=false 字段添加失败的专项用例(空对象 {} 变异即命中
    // 此路径:修复前 check_reply_frames 只看子串,会把 {} 当成合法帧)。
    envelope_double_failure_case("错误应答 ok 字段失败",
                                 JSON_OP_ADD_ITEM_OBJECT, 2, JSON_OP_ADD_BOOL,
                                 2);
    envelope_double_failure_case("错误应答序列化失败", JSON_OP_ADD_ITEM_OBJECT,
                                 2, JSON_OP_PRINT, 1);
    envelope_double_failure_case("错误应答 err 字段失败",
                                 JSON_OP_ADD_ITEM_OBJECT, 2, JSON_OP_SSID, 2);

    scan_envelope_repeat_and_recover();
    reply_contract_cases();

    check_zero("应答封装失败处理");
}

// ---- 场景 8:配网页退出、重进与资源回收 ----
//
// 注意范围:本场景覆盖"空闲态"退出(任务停靠在队列接收时调用 end()),
// 不代表覆盖强制终止(断电/复位/任务被外部删除)路径的资源回收。

static void scenario_page_exit_reenter(void)
{
    // 会话中(已认证)产生一次多帧应答,再真实退出配网页。
    hangup_and_connect(0);
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    run_and_park();
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    rx_json(k_provinces_cmd, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    assert(test_prov_live_tasks() == 1);
    assert(test_prov_live_queues() == 1);
    assert(test_prov_live_sems() == 2);  // 退出信号量 + 生命周期锁(init 常驻)
    assert(test_prov_live_mbufs() == 0);  // 帧全部释放

    // 真实退出:end() 需要队列唤醒语义(任务停靠于队列接收)。
    test_prov_set_auto_wake(true);
    suite_prov_ble_end();
    test_prov_set_auto_wake(false);
    raise_disconnect(0, 19);

    // 本次启动获得的资源全部回收;只保留 init 期常驻的生命周期锁。
    assert(test_prov_live_tasks() == 0);
    assert(test_prov_live_queues() == 0);
    assert(test_prov_live_sems() == 1);
    assert(test_prov_live_mbufs() == 0);

    // 重进:新会话必须重新连接/订阅/hello,通知路径可用。
    assert(suite_prov_ble_begin("135790") == ESP_OK);
    assert(test_prov_live_tasks() == 1);
    assert(test_prov_live_queues() == 1);
    assert(test_prov_live_sems() == 2);
    assert(test_prov_wait_park() == PARK_WAIT_MS);
    raise_connect(0);
    run_and_park();
    raise_subscribe(0, true);
    raise_mtu(0, 247);
    run_and_park();
    unsigned serial = test_prov_conn_serial(0);
    size_t mark = g_frame_count;
    rx_json(k_hello_ok, SUITE_PROV_PAYLOAD_MAX);
    run_and_park();
    tx_msg_t msgs[MAX_TX_MSGS];
    size_t n = collect_messages(mark, g_frame_count, msgs, MAX_TX_MSGS);
    assert(count_messages_conn(msgs, n, serial, "\"ok\":true") == 1);
    assert(any_protected_conn(msgs, n, serial));  // 状态推送恢复
    for (size_t i = mark; i < g_frame_count; i++) {
        assert(g_frames[i].conn == serial);
        assert(g_frames[i].len <= 247 - 3);
    }
    assert(test_prov_live_mbufs() == 0);
}

int main(void)
{
    // 初始化:一次性 NimBLE 桩 + 配网页会话。
    // 真实分配器记账必须在任何 cJSON 使用之前安装:根对象、子树、键/值
    // 字符串与序列化缓冲都经同一组钩子分配/释放,存活块数可直接比较。
    test_json_hooks_install();
    s_tx_val_handle = 5;
    g_rx_chr.uuid = &k_uuid_rx.u;
    g_rx_chr.access_cb = gatt_access;
    assert(mtx_init(&s_host_lock, mtx_plain) == thrd_success);
    assert(cnd_init(&s_host_cv) == thrd_success);
    test_prov_set_frame_sink(frame_sink);
    suite_time_sync_succeeded(&s_tracker, suite_connection_now());
    assert(suite_prov_ble_init() == ESP_OK);
    assert(suite_prov_ble_begin("135790") == ESP_OK);
    assert(test_prov_wait_park() == PARK_WAIT_MS);

    scenario_scan_reconnect_isolation();
    scenario_queue_full_recovery();
    scenario_mtu_isolation_and_framing();
    scenario_mid_send_and_precommit_switch();
    scenario_wrong_pin_and_recovery();
    scenario_notify_commit_switch_isolation();
    scenario_scan_result_ownership();
    scenario_reply_envelope_failures();
    scenario_page_exit_reenter();

    puts("BLE provisioning cross-connection isolation tests: PASS");
    return 0;
}
