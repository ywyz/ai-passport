/* Host tests for the pure travel modules (D1b). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "passport_keys.h"
#include "passport_pack.h"
#include "passport_events.h"

/* ---------------------------------------------------------------- keys */
static struct passport_key_state key_states[3];

static char feed(int button, bool down, uint32_t tick)
{
    return passport_key_feed(&key_states[button], button, down, tick);
}

static void test_hold_suppresses_click(void)
{
    memset(key_states, 0, sizeof(key_states));
    int button = PASSPORT_BTN_OK;
    assert(feed(button, true, 0) == PASSPORT_EVT_PRESS);
    assert(feed(button, false, 500) == PASSPORT_EVT_CLICK); /* short */
    assert(feed(button, true, 1000) == PASSPORT_EVT_PRESS);
    assert(feed(button, false, 700 + 1000 + 5) == PASSPORT_EVT_RELEASE);
    /* no CLICK after hold release */
}

static void test_hold_start_at_700(void)
{
    assert(feed(PASSPORT_BTN_OK, true, 2000) == PASSPORT_EVT_PRESS);
    assert(feed(PASSPORT_BTN_OK, false, 2695) == PASSPORT_EVT_CLICK);
    assert(feed(PASSPORT_BTN_OK, true, 3000) == PASSPORT_EVT_PRESS);
    assert(feed(PASSPORT_BTN_OK, false, 3699) == PASSPORT_EVT_CLICK);
    assert(feed(PASSPORT_BTN_OK, true, 4000) == PASSPORT_EVT_PRESS);
    assert(feed(PASSPORT_BTN_OK, false, 4700) == PASSPORT_EVT_RELEASE);
}

static void test_repeat_acceleration_bounded(void)
{
    memset(key_states, 0, sizeof(key_states));
    assert(feed(PASSPORT_BTN_UP, true, 100) == PASSPORT_EVT_PRESS);
    (void)feed(PASSPORT_BTN_UP, true, 540);  /* early: no repeat yet */
    assert(feed(PASSPORT_BTN_UP, true, 545) == PASSPORT_EVT_NONE);
    assert(feed(PASSPORT_BTN_UP, true, 549) == PASSPORT_EVT_NONE);
    /* HOLD_START then repeats at T1 period (180 ms) */
    assert(feed(PASSPORT_BTN_UP, true, 820) == PASSPORT_EVT_HOLD_START);
    assert(feed(PASSPORT_BTN_UP, true, 1000) == PASSPORT_EVT_REPEAT);
    assert(feed(PASSPORT_BTN_UP, true, 1250) == PASSPORT_EVT_REPEAT);
    /* stage 2: 90 ms after 1.5 s held */
    assert(feed(PASSPORT_BTN_UP, true, 4601) == PASSPORT_EVT_REPEAT);
    assert(feed(PASSPORT_BTN_UP, true, 4640) == PASSPORT_EVT_NONE);
    assert(feed(PASSPORT_BTN_UP, true, 4690) == PASSPORT_EVT_REPEAT);
    /* release after hold: no click */
    assert(feed(PASSPORT_BTN_UP, false, 4695) == PASSPORT_EVT_RELEASE);
    /* stage 3: 60 ms after 3 s held (fresh press cycle) */
    assert(feed(PASSPORT_BTN_UP, true, 5000) == PASSPORT_EVT_PRESS);
    assert(feed(PASSPORT_BTN_UP, true, 8000) == PASSPORT_EVT_HOLD_START);
    assert(feed(PASSPORT_BTN_UP, true, 8030) == PASSPORT_EVT_NONE);
    assert(feed(PASSPORT_BTN_UP, true, 8060) == PASSPORT_EVT_REPEAT);
    assert(feed(PASSPORT_BTN_UP, true, 8090) == PASSPORT_EVT_NONE);
    assert(feed(PASSPORT_BTN_UP, true, 8120) == PASSPORT_EVT_REPEAT);
    assert(feed(PASSPORT_BTN_UP, false, 8160) == PASSPORT_EVT_RELEASE);
}

/* --------------------------------------------------------------- packs */
static void test_pack_manifest_parse_and_reject(void)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"schema_version\":1,\"pack_id\":\"fixture-js-trip-001\","
        "\"revision\":1,\"kind\":\"guide\",\"fixture\":true,"
        "\"total_bytes\":26,"
        "\"glyph_inventory\":[%d],"
        "\"route_stops\":{\"route_id\":\"fixture-route-001\","
        "\"stop_ids\":[\"stop-001\"]},"
        "\"files\":[{\"path\":\"text/stop-001.txt\",\"file_id\":\"fabc\","
        "\"size_bytes\":26,\"sha256\":"
        "\"5f8a2f00d4dcd5a30a3a9e1076ce478aeebe3c40e6c5a6fc84d1a4b7e5d1e5c8\","
        "\"media_type\":\"text/plain\"}]}",
        0x752A);
    struct passport_pack_manifest m;
    assert(passport_pack_parse_manifest(buf, strlen(buf), &m) == 0);
    assert(m.revision == 1 && m.fixture && m.route_stop_count == 1);
    assert(strcmp(m.pack_id, "fixture-js-trip-001") == 0);
    assert(strcmp(m.files[0].path, "text/stop-001.txt") == 0);
    assert(strcmp(m.files[0].file_id, "fabc") == 0);
    assert(m.files[0].size_bytes == 26);
    /* traversal rejections (A03) */
    const char *bad_paths[] = {
        "../etc/passwd", "/abs/path", "data\\x.txt", "a/../b", "./x",
        "other/x.txt", "data//double", "data/../y.txt", "data/.hidden",
        "",
    };
    for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
        assert(passport_pack_check_path(bad_paths[i], strlen(bad_paths[i]))
               == PASSPORT_PACK_BAD_PATH);
    }
    assert(passport_pack_check_path("text/a.txt", strlen("text/a.txt")) == 0);
    assert(passport_pack_check_path("fonts/glyphs.json", 15) == 0);
    /* digest */
    uint8_t bin[32];
    static const char small[65] = "00000000000000000000000000000000000000000000000000000000000000";
    assert(passport_pack_digest_hex_to_bin(small, 63, bin) == -1); /* short */
    assert(passport_pack_digest_hex_to_bin(
               "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824", 64, bin) == 0); /* upper case accepted */
    assert(passport_pack_digest_hex_to_bin(
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 64, bin) == 0);
    /* preflight: no space */
    assert(passport_pack_preflight(100, 10, 100) == PASSPORT_PACK_NO_SPACE);
    assert(passport_pack_preflight(100, 10, 110) == PASSPORT_PACK_OK);
}

static void test_glyph_check(void)
{
    static const uint32_t inventory[] = {0x752A};
    struct passport_glyph_set font = {inventory, 1};
    uint32_t missing = 0;
    /* 甪 U+752A covered */
    const uint8_t covered[] = {0xE7, 0x94, 0xAA};
    assert(passport_glyph_check_utf8(font, covered, 3, &missing) == 0);
    /* U+2A302 (F0 AA 8C 82) not covered */
    const uint8_t missing_seq[] = {0xF0, 0xAA, 0x8C, 0x82};
    assert(passport_glyph_check_utf8(font, missing_seq, 4, &missing)
           == PASSPORT_PACK_GLYPH_MISSING);
    assert(missing == 0x2A302);
    /* invalid utf8 rejected */
    const uint8_t invalid[] = {0xE7, 0x94};
    assert(passport_glyph_check_utf8(font, invalid, 2, &missing)
           == PASSPORT_PACK_BAD_SCHEMA);
    /* ASCII covered by the same call */
}

/* -------------------------------------------------------------- events */
static char kv_data[1024][64];
static uint8_t kv_blob[1024][PASSPORT_KV_MAX_VALUE];
static size_t kv_len[256];
static size_t kv_used;

static int kv_get(void *ctx, const char *key, void *out, size_t cap,
                  size_t *out_len)
{
    (void)ctx;
    for (size_t i = 0; i < kv_used; ++i) {
        if (strcmp(kv_data[i], key) == 0) {
            size_t take = kv_len[i] < cap ? kv_len[i] : cap;
            memcpy(out, kv_blob[i], take);
            *out_len = take;
            return 0;
        }
    }
    return -1;
}

static int kv_set(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < kv_used; ++i) {
        if (strcmp(kv_data[i], key) == 0) {
            memcpy(kv_blob[i], data, len);
            kv_len[i] = len;
            return 0;
        }
    }
    if (kv_used >= 1024)
        return -1;
    snprintf(kv_data[kv_used], sizeof(kv_data[kv_used]), "%s", key);
    memcpy(kv_blob[kv_used], data, len);
    kv_len[kv_used] = len;
    ++kv_used;
    return 0;
}

static int kv_del(void *ctx, const char *key)
{
    (void)ctx;
    for (size_t i = 0; i < kv_used; ++i) {
        if (strcmp(kv_data[i], key) == 0) {
            kv_len[i] = 0;
            kv_data[i][0] = '\0';
            return 0;
        }
    }
    return -1;
}

static int kv_list(void *ctx, const char *prefix,
                   int (*cb)(void *, const char *), void *cbctx)
{
    (void)ctx;
    (void)prefix;
    (void)cb;
    (void)cbctx;
    return 0;
}

static const struct passport_kv test_kv = {kv_get, kv_set, kv_del, kv_list, NULL};

static void test_events_journal(void)
{
    kv_used = 0;
    memset(kv_len, 0, sizeof(kv_len));
    assert(passport_events_open(&test_kv) == 0);
    struct passport_event e0;
    memset(&e0, 0, sizeof(e0));
    strcpy(e0.context_id, "fixture-route-001");
    strcpy(e0.target_id, "stop-001");
    e0.op = PASSPORT_EV_GUIDE_PROGRESS_SET;
    e0.value = 1;
    e0.base_revision = 0;
    assert(passport_events_enqueue(&e0) == PASSPORT_EV_OK);
    assert(passport_events_pending() == 1);
    /* identical retry: dedup, no second entry */
    struct passport_event e0b = e0;
    assert(passport_events_enqueue(&e0b) == PASSPORT_EV_OK);
    assert(passport_events_pending() == 1);
    /* durable reload (power loss cycle) */
    assert(passport_events_open(&test_kv) == 0);
    assert(passport_events_pending() == 1);
    /* queue full: reject at capacity without losing history (A03/A04) */
    for (size_t i = 0; i < PASSPORT_EVENT_CAPACITY + 1; ++i) {
        struct passport_event e;
        memset(&e, 0, sizeof(e));
        char id[16];
        snprintf(id, sizeof(id), "stop-%03zu", i % 1000);
        strcpy(e.target_id, id);
        strcpy(e.context_id, "fixture-route-001");
        e.op = PASSPORT_EV_GUIDE_PROGRESS_SET;
        e.value = 1;
        int rc = passport_events_enqueue(&e);
        if (rc == PASSPORT_EV_FULL)
            break;
        assert(rc == PASSPORT_EV_OK);
    }
    assert(passport_events_pending() == PASSPORT_EVENT_CAPACITY);
    struct passport_event extra;
    memset(&extra, 0, sizeof(extra));
    strcpy(extra.target_id, "final");
    strcpy(extra.context_id, "fixture-route-001");
    extra.op = PASSPORT_EV_GUIDE_PROGRESS_SET;
    assert(passport_events_enqueue(&extra) == PASSPORT_EV_FULL);
    /* take all, ack, reload, stable */
    struct passport_event taken[PASSPORT_EVENT_CAPACITY];
    size_t n = passport_events_take(taken, PASSPORT_EVENT_CAPACITY);
    assert(n > 0);
    const char *ids[PASSPORT_EVENT_CAPACITY];
    for (size_t i = 0; i < n; ++i)
        ids[i] = taken[i].event_id;
    size_t acked = 0;
    assert(passport_events_ack(ids, n, &acked) == PASSPORT_EV_OK);
    assert(acked == n && passport_events_pending() == 0);
    assert(passport_events_open(&test_kv) == 0);
    assert(passport_events_pending() == 0);
    /* record state persisted and reloaded */
    assert(passport_events_record_save("stop-001", true, 2) == 0);
    bool value = false;
    uint32_t revision = 0;
    assert(passport_events_open(&test_kv) == 0);
    assert(passport_events_record_state("stop-001", &value, &revision) == 0);
    assert(value && revision == 2);
}

int main(void)
{
    test_hold_suppresses_click();
    test_hold_start_at_700();
    test_repeat_acceleration_bounded();
    test_pack_manifest_parse_and_reject();
    test_glyph_check();
    test_events_journal();
    printf("passport unit tests: OK\n");
    return 0;
}
