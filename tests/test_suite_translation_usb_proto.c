/* Strict USB provisioning framing, parsing, session and bounded runner. */
#include "suite_translation_usb_proto.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned apply_calls;
static suite_translation_apply_t apply_result;
static char last_origin[193], last_token[129];

static suite_translation_apply_t apply_capture(const char *origin, const char *token, void *ctx)
{
    (void)ctx;
    apply_calls++;
    strcpy(last_origin, origin);
    strcpy(last_token, token);
    return apply_result;
}

static unsigned cancels;
static bool cancel_flag;
static bool cancelled_cb(void *ctx)
{
    (void)ctx;
    cancels++;
    return cancel_flag;
}

typedef struct {
    const char *input;
    size_t input_length, input_at;
    char output[4096];
    size_t output_length;
    int64_t now;
    int64_t read_step;
    int fail_read;
    bool set_cancel_after_read;
    bool expire_after_read;
    size_t chunk;
} script_t;

static int script_read(void *ctx, void *buffer, size_t capacity, uint32_t timeout_ms)
{
    (void)timeout_ms;
    script_t *script = ctx;
    script->now += script->read_step;
    if (script->fail_read) return -1;
    size_t n = script->input_length - script->input_at;
    if (n > capacity) n = capacity;
    if (n > script->chunk) n = script->chunk;
    memcpy(buffer, script->input + script->input_at, n);
    script->input_at += n;
    if (script->input_at == script->input_length) {
        if (script->set_cancel_after_read) cancel_flag = true;
        if (script->expire_after_read) script->now = 60001;
    }
    return (int)n;
}
static int script_write(void *ctx, const void *buffer, size_t bytes, uint32_t timeout_ms)
{
    (void)timeout_ms;
    script_t *script = ctx;
    assert(script->output_length + bytes <= sizeof(script->output));
    memcpy(script->output + script->output_length, buffer, bytes);
    script->output_length += bytes;
    return (int)bytes;
}
static int64_t script_now(void *ctx) { return ((script_t *)ctx)->now; }

static const char *valid_frame =
    "{\"schema\":1,\"operation\":\"set_translation_config\","
    "\"request_id\":\"0123456789abcdef0123456789abcdef\","
    "\"origin\":\"https://translate.example.test\","
    "\"token\":\"translation_token_0123456789abcdef\"}\n";

/* Link-only NVS surface for the real validation source. */
esp_err_t nvs_open_from_partition(const char *part, const char *space, int mode, nvs_handle_t *handle)
{
    (void)part; (void)space; (void)mode; (void)handle;
    return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *bytes)
{ (void)handle; (void)key; (void)out; (void)bytes; return ESP_FAIL; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *bytes)
{ (void)handle; (void)key; (void)out; (void)bytes; return ESP_FAIL; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t bytes)
{ (void)handle; (void)key; (void)value; (void)bytes; return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_FAIL; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ (void)handle; (void)key; return ESP_FAIL; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{ (void)type; (void)subtype; (void)name; return NULL; }
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *part, nvs_sec_cfg_t *cfg)
{ (void)part; (void)cfg; return ESP_FAIL; }
esp_err_t nvs_flash_secure_init_partition(const char *name, nvs_sec_cfg_t *cfg)
{ (void)name; (void)cfg; return ESP_FAIL; }
void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)

static void reset_script(script_t *script, const char *input, int64_t now)
{
    memset(script, 0, sizeof(*script));
    script->input = input;
    script->input_length = strlen(input);
    script->now = now;
    script->read_step = 1;
    script->chunk = 7;
    script->set_cancel_after_read = false;
    script->expire_after_read = false;
    apply_calls = 0;
    cancels = 0;
    cancel_flag = false;
    apply_result = SUITE_TRANSLATION_APPLY_OK;
    memset(last_origin, 0, sizeof(last_origin));
    memset(last_token, 0, sizeof(last_token));
}

static void check_ack(const script_t *script, const char *status, bool verified)
{
    assert(script->output_length > 1);
    assert(script->output[script->output_length - 1] == '\n');
    const char *raw = script->output;
    assert(strstr(raw, "\"request_id\":\"0123456789abcdef0123456789abcdef\""));
    assert(strstr(raw, status));
    assert(strstr(raw, verified ? "\"verified\":true" : "\"verified\":false"));
    assert(!strstr(raw, "translate.example.test"));
    assert(!strstr(raw, "translation_token"));
    assert(strstr(raw, "{\"request_id\""));
}

int main(void)
{
    /* Framing across arbitrary fragments, oversize discard and recovery. */
    suite_translation_usb_framer_t framer;
    suite_translation_usb_framer_reset(&framer);
    unsigned ready = 0;
    for (size_t i = 0; i < strlen(valid_frame); i++)
        if (suite_translation_usb_framer_push(&framer, valid_frame[i]) ==
            SUITE_TRANSLATION_USB_FRAME_READY) ready++;
    assert(ready == 1 && !strcmp(framer.line,
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://translate.example.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}"));
    suite_translation_usb_framer_reset(&framer);
    bool oversize = false;
    for (unsigned i = 0; i < SUITE_TRANSLATION_USB_FRAME_MAX + 8; i++)
        if (suite_translation_usb_framer_push(&framer, 'x') ==
            SUITE_TRANSLATION_USB_FRAME_OVERSIZE) oversize = true;
    assert(oversize);
    assert(suite_translation_usb_framer_push(&framer, '\n') == SUITE_TRANSLATION_USB_FRAME_NONE);
    for (size_t i = 0; i < strlen(valid_frame); i++)
        if (suite_translation_usb_framer_push(&framer, valid_frame[i]) ==
            SUITE_TRANSLATION_USB_FRAME_READY) ready++;
    assert(ready == 2);

    char crlf_frame[2048];
    snprintf(crlf_frame, sizeof(crlf_frame),
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://translate.example.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}\r\n");
    suite_translation_usb_framer_reset(&framer);
    ready = 0;
    for (size_t i = 0; i < strlen(crlf_frame); i++)
        if (suite_translation_usb_framer_push(&framer, crlf_frame[i]) ==
            SUITE_TRANSLATION_USB_FRAME_READY) ready++;
    assert(ready == 1 && !strchr(framer.line, '\r'));

    /* Parser accepts only the exact request shape. */
    suite_translation_usb_request_t request;
    assert(suite_translation_usb_parse(valid_frame, strlen(valid_frame) - 1, &request) ==
           SUITE_TRANSLATION_USB_PARSE_REQUEST);
    assert(!strcmp(request.request_id, "0123456789abcdef0123456789abcdef"));
    const char *invalid[] = {
        "{\"schema\":2,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\"}",
        "{\"schema\":1,\"operation\":\"other\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\"}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"http://a.test\",\"token\":\"translation_token_0123456789abcdef\"}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"short\"}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\",\"extra\":1}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\"}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789ABCDEF0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\"}",
        "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\"}tail",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        suite_translation_usb_parse_t result = suite_translation_usb_parse(invalid[i], strlen(invalid[i]), &request);
        assert(result != SUITE_TRANSLATION_USB_PARSE_REQUEST);
        /* Syntactically strict objects with a valid ID get an error reply;
           duplicate keys/trailing bytes are untrusted noise and are ignored. */
        if (i < 5) assert(result == SUITE_TRANSLATION_USB_PARSE_INVALID);
    }
    const char *escaped_nul =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789abcdef\\u0000suffix\"}";
    assert(suite_translation_usb_parse(escaped_nul, strlen(escaped_nul), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    assert(!strcmp(request.request_id, "0123456789abcdef0123456789abcdef"));
    const char *nul_key =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789abcdef\","
        "\"origin\\u0000x\":1}";
    assert(suite_translation_usb_parse(nul_key, strlen(nul_key), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    const char *trailing =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}tail";
    assert(suite_translation_usb_parse(trailing, strlen(trailing), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    const char *escaped_dup =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789abcdef\","
        "\"\\u0074oken\":\"translation_token_0123456789abcdef\"}";
    assert(suite_translation_usb_parse(escaped_dup, strlen(escaped_dup), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);

    const char *escaped_value =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"\\u0061aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
    assert(suite_translation_usb_parse(escaped_value, strlen(escaped_value), &request) ==
           SUITE_TRANSLATION_USB_PARSE_REQUEST);
    assert(request.token[0] == 'a' && strlen(request.token) == 33);

    const char *escaped_key =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"\\u0074oken\":\"translation_token_0123456789abcdef\"}";
    assert(suite_translation_usb_parse(escaped_key, strlen(escaped_key), &request) ==
           SUITE_TRANSLATION_USB_PARSE_REQUEST);

    const char *escaped_slashes =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https:\\/\\/translate.example.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}";
    assert(suite_translation_usb_parse(escaped_slashes, strlen(escaped_slashes), &request) ==
           SUITE_TRANSLATION_USB_PARSE_REQUEST);
    assert(!strcmp(request.origin, "https://translate.example.test"));

    const char *escaped_quote_backslash =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789abcdef\","
        "\"extra\":\"a\\\"b\\\\c\"}";
    assert(suite_translation_usb_parse(escaped_quote_backslash, strlen(escaped_quote_backslash), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);

    const char *bad_escape =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789\\qabcdef\"}";
    assert(suite_translation_usb_parse(bad_escape, strlen(bad_escape), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    const char *short_unicode =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789\\u12\"}";
    assert(suite_translation_usb_parse(short_unicode, strlen(short_unicode), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);

    const char *raw_cr_token =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://a.test\","
        "\"token\":\"translation_token_0123456789\rabcdef\"}\n";
    assert(suite_translation_usb_parse(raw_cr_token, strlen(raw_cr_token) - 1, &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);

    char deep[SUITE_TRANSLATION_USB_FRAME_MAX + 1];
    size_t deep_bytes = (size_t)sprintf(deep,
        "{\"schema\":1,\"request_id\":\"0123456789abcdef0123456789abcdef\",\"nested\":");
    for (unsigned i = 0; i < 400; i++) deep[deep_bytes++] = '[';
    deep[deep_bytes++] = '0';
    for (unsigned i = 0; i < 400; i++) deep[deep_bytes++] = ']';
    deep[deep_bytes++] = '}';
    deep[deep_bytes++] = '\n';
    deep[deep_bytes] = 0;
    assert(deep_bytes <= SUITE_TRANSLATION_USB_FRAME_MAX);
    assert(suite_translation_usb_parse(deep, deep_bytes - 1, &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    assert(!strcmp(request.request_id, "0123456789abcdef0123456789abcdef"));

    char bad_utf8[] = "{\"schema\":1,\"operation\":\"set_translation_config\",\"request_id\":\"0123456789abcdef0123456789abcdef\",\"origin\":\"https://a.test\",\"token\":\"translation_token_0123456789abcdef\xc0\xaf\"}";
    assert(suite_translation_usb_parse(bad_utf8, strlen(bad_utf8), &request) ==
           SUITE_TRANSLATION_USB_PARSE_INVALID);
    assert(!strcmp(request.request_id, "0123456789abcdef0123456789abcdef"));
    char nul_frame[] = "{\"schema\":1}\0tail";
    assert(suite_translation_usb_parse(nul_frame, sizeof(nul_frame) - 1, &request) ==
           SUITE_TRANSLATION_USB_PARSE_IGNORE);
    char too_long[SUITE_TRANSLATION_USB_FRAME_MAX + 2];
    memset(too_long, 'x', sizeof(too_long));
    assert(suite_translation_usb_parse(too_long, sizeof(too_long), &request) ==
           SUITE_TRANSLATION_USB_PARSE_IGNORE);

    /* Session duplicate/conflict semantics never consume a second write. */
    suite_translation_usb_session_t session;
    suite_translation_usb_session_reset(&session);
    assert(suite_translation_usb_session_feed(&session, valid_frame, strlen(valid_frame) - 1,
        &request) == SUITE_TRANSLATION_USB_STEP_APPLY);
    suite_translation_usb_session_complete(&session, SUITE_TRANSLATION_USB_STATUS_OK, true);
    assert(suite_translation_usb_session_feed(&session, valid_frame, strlen(valid_frame) - 1,
        &request) == SUITE_TRANSLATION_USB_STEP_REPLY);
    assert(session.status == SUITE_TRANSLATION_USB_STATUS_OK && session.verified);
    const char *conflict =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"https://other.example.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}";
    assert(suite_translation_usb_session_feed(&session, conflict, strlen(conflict),
        &request) == SUITE_TRANSLATION_USB_STEP_REPLY);
    assert(session.status == SUITE_TRANSLATION_USB_STATUS_INVALID && !session.verified);
    const char *different =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"ffffffffffffffffffffffffffffffff\","
        "\"origin\":\"https://translate.example.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}";
    assert(suite_translation_usb_session_feed(&session, different, strlen(different),
        &request) == SUITE_TRANSLATION_USB_STEP_REPLY);
    assert(session.status == SUITE_TRANSLATION_USB_STATUS_BUSY);

    /* Ack shape is exactly request ID, status and verified. */
    char ack[160];
    size_t ack_length = suite_translation_usb_ack(ack, sizeof(ack),
        "0123456789abcdef0123456789abcdef", SUITE_TRANSLATION_USB_STATUS_STORAGE, false);
    assert(ack_length && !strcmp(ack,
        "{\"request_id\":\"0123456789abcdef0123456789abcdef\",\"status\":\"STORAGE\",\"verified\":false}\n"));
    assert(!suite_translation_usb_ack(ack, sizeof(ack), "short", SUITE_TRANSLATION_USB_STATUS_OK, true));

    /* Real runner applies exactly once, acknowledges, and never echoes input. */
    suite_translation_usb_transport_t transport = {
        .read = script_read, .write = script_write, .now_ms = script_now};
    script_t script;
    reset_script(&script, valid_frame, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1 && !strcmp(last_origin, "https://translate.example.test"));
    assert(!strcmp(last_token, "translation_token_0123456789abcdef"));
    assert(script.output_length > 1 && !strstr(script.output, "translation_token"));
    check_ack(&script, "\"status\":\"OK\"", true);

    /* Fragmented input is assembled before any apply. */
    reset_script(&script, valid_frame, 0);
    script.chunk = 1;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1);

    /* An invalid request with an ID consumes the one bounded attempt; a new
       window is required for the corrected request. */
    const char *bad_origin =
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"origin\":\"http://bad.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}\n";
    char two_frames[4096];
    snprintf(two_frames, sizeof(two_frames), "%s%s", bad_origin, valid_frame);
    reset_script(&script, two_frames, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 0);
    assert(strstr(script.output, "\"status\":\"INVALID\""));
    assert(!strstr(script.output, "\"status\":\"OK\""));
    assert(script.input_at < script.input_length); /* valid frame wait was not applied */
    reset_script(&script, valid_frame, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1);
    check_ack(&script, "\"status\":\"OK\"", true);

    /* Unrelated console/log lines are ignored without consuming the window. */
    char logged[4096];
    snprintf(logged, sizeof(logged), "I (123) suite_weather: update complete\n%s", valid_frame);
    reset_script(&script, logged, 0);
    script.chunk = 3;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1);

    /* Two identical frames in one window still produce one write and one ack. */
    char duplicate[4096];
    snprintf(duplicate, sizeof(duplicate), "%s%s", valid_frame, valid_frame);
    reset_script(&script, duplicate, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1);
    assert(script.output_length == strlen(script.output));
    assert(!strcmp(script.output,
        "{\"request_id\":\"0123456789abcdef0123456789abcdef\",\"status\":\"OK\",\"verified\":true}\n"));

    /* Apply failures map to distinct statuses and never claim verification. */
    reset_script(&script, valid_frame, 0);
    apply_result = SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    check_ack(&script, "\"status\":\"STORAGE\"", false);
    reset_script(&script, valid_frame, 0);
    apply_result = SUITE_TRANSLATION_APPLY_VERIFY_UNKNOWN;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    check_ack(&script, "\"status\":\"UNVERIFIED\"", false);
    reset_script(&script, valid_frame, 0);
    apply_result = SUITE_TRANSLATION_APPLY_UNCHANGED;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    check_ack(&script, "\"status\":\"OK\"", true);

    /* The actual runner rejects a raw CR inside a token without applying, and
       accepts a valid CRLF-terminated frame. */
    reset_script(&script, raw_cr_token, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 0);
    check_ack(&script, "\"status\":\"INVALID\"", false);
    reset_script(&script, crlf_frame, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 1);
    check_ack(&script, "\"status\":\"OK\"", true);

    /* Deeply nested input is rejected by the bounded flat precheck before cJSON
       recursion and still produces a known-ID INVALID reply. */
    reset_script(&script, deep, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 0);
    check_ack(&script, "\"status\":\"INVALID\"", false);

    /* A malformed frame with a safely known ID gets an INVALID reply, not an
       empty-ID/IO_ERROR path, and never applies. */
    char known_bad[512];
    snprintf(known_bad, sizeof(known_bad),
        "{\"schema\":1,\"operation\":\"set_translation_config\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"origin\":\"http://bad.test\","
        "\"token\":\"translation_token_0123456789abcdef\"}\n");
    reset_script(&script, known_bad, 0);
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_COMPLETED);
    assert(apply_calls == 0);
    check_ack(&script, "\"status\":\"INVALID\"", false);

    /* Cancellation or expiry observed during the read closes before apply. */
    reset_script(&script, valid_frame, 0);
    script.set_cancel_after_read = true;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_CANCELLED);
    assert(apply_calls == 0 && script.output_length == 0);
    reset_script(&script, valid_frame, 0);
    script.expire_after_read = true;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_TIMEOUT);
    assert(apply_calls == 0 && script.output_length == 0);

    /* Window expiry, cancellation and I/O failure never apply. */
    reset_script(&script, valid_frame, 0);
    script.read_step = 1000;
    script.input = NULL;
    script.input_length = 0;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_TIMEOUT);
    assert(apply_calls == 0 && script.output_length == 0);

    reset_script(&script, valid_frame, 0);
    cancel_flag = true;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_CANCELLED);
    assert(apply_calls == 0 && cancels > 0 && script.output_length == 0);

    reset_script(&script, valid_frame, 0);
    script.fail_read = true;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 60000) ==
           SUITE_TRANSLATION_USB_RUN_IO_ERROR);
    assert(apply_calls == 0);

    /* A larger requested window is capped at 60 s: a fragmented frame cannot
       stretch the window past the cap and no write occurs after it. */
    reset_script(&script, valid_frame, 0);
    script.read_step = 60000;
    transport.ctx = &script;
    assert(suite_translation_usb_run(&transport, apply_capture, NULL, cancelled_cb, NULL, 6000000) ==
           SUITE_TRANSLATION_USB_RUN_TIMEOUT);
    assert(apply_calls == 0 && script.output_length == 0);

    puts("USB translation config framing / parser / session / runner bounds: PASS");
    return 0;
}
