#include "suite_translation_usb_proto.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define USB_KEYS 5U

static bool hex32_n(const char *s, size_t n)
{
    if (!s || n != 32U) return false;
    for (unsigned i = 0; i < 32U; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool hex32(const char *s) { return s && hex32_n(s, strlen(s)); }

/* cJSON decodes \u0000 to an embedded NUL, which makes strlen/strcmp accept a
   valid-looking prefix followed by hidden bytes. Reject the escape before any
   parse or write. */
static bool contains_nul_escape(const char *line, size_t bytes)
{
    for (size_t i = 0; i + 6 <= bytes; i++)
        if (line[i] == '\\' && line[i + 1] == 'u' &&
            line[i + 2] == '0' && line[i + 3] == '0' &&
            line[i + 4] == '0' && line[i + 5] == '0') return true;
    return false;
}

/* Best-effort ID extraction for a frame that failed strict parsing. Only an
   unambiguous compact 32-lowercase-hex request_id is reflected. */
static bool raw_request_id(const char *line, size_t bytes, char out[33])
{
    const char *needle = "\"request_id\":\"";
    size_t needle_length = strlen(needle);
    unsigned found = 0;
    char candidate[33] = {0};
    for (size_t i = 0; i + needle_length + 33U <= bytes; i++) {
        if (memcmp(line + i, needle, needle_length)) continue;
        const char *id = line + i + needle_length;
        if (id[32] != '"' || !hex32_n(id, 32)) continue;
        if (found++) return false;
        memcpy(candidate, id, 32);
        candidate[32] = 0;
    }
    if (found != 1) return false;
    memcpy(out, candidate, 33);
    return true;
}

static bool utf8_valid(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        unsigned more;
        uint32_t value;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; value = c & 0x1fU; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; value = c & 0x0fU; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; value = c & 0x07U; }
        else return false;
        for (unsigned k = 0; k < more; k++) {
            if (i >= n || (s[i] & 0xc0U) != 0x80U) return false;
            value = (value << 6) | (s[i++] & 0x3fU);
        }
        if ((more == 1 && value < 0x80U) || (more == 2 && value < 0x800U) ||
            (more == 3 && value < 0x10000U) || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

void suite_translation_usb_framer_reset(suite_translation_usb_framer_t *framer)
{
    if (!framer) return;
    memset(framer, 0, sizeof(*framer));
}

suite_translation_usb_frame_t suite_translation_usb_framer_push(
    suite_translation_usb_framer_t *framer, char byte)
{
    if (!framer) return SUITE_TRANSLATION_USB_FRAME_NONE;
    if (byte == '\n') {
        if (framer->discarding) {
            framer->discarding = false;
            framer->used = 0;
            return SUITE_TRANSLATION_USB_FRAME_NONE;
        }
        /* CRLF is accepted; an embedded CR is retained so the strict parser
           rejects the line instead of silently deleting a byte. */
        if (framer->used && framer->line[framer->used - 1] == '\r') framer->used--;
        if (!framer->used) {
            framer->used = 0;
            return SUITE_TRANSLATION_USB_FRAME_NONE;
        }
        framer->line[framer->used] = 0;
        return SUITE_TRANSLATION_USB_FRAME_READY;
    }
    if (framer->discarding) return SUITE_TRANSLATION_USB_FRAME_NONE;
    if (framer->used >= SUITE_TRANSLATION_USB_FRAME_MAX) {
        framer->discarding = true;
        framer->used = 0;
        return SUITE_TRANSLATION_USB_FRAME_OVERSIZE;
    }
    framer->line[framer->used++] = byte;
    return SUITE_TRANSLATION_USB_FRAME_NONE;
}

/* Iterative, allocation-free shape check for the only frame this protocol
   accepts: one flat object with exactly five scalar (string/number) members.
   Running it before cJSON bounds parser recursion; a 1,024-byte frame can
   otherwise contain hundreds of nested arrays. */
static void flat_skip_ws(const char *line, size_t bytes, size_t *at)
{
    while (*at < bytes && (line[*at] == ' ' || line[*at] == '\t' ||
           line[*at] == '\r' || line[*at] == '\n')) (*at)++;
}

static bool flat_skip_string(const char *line, size_t bytes, size_t *at)
{
    if (*at >= bytes || line[*at] != '"') return false;
    (*at)++;
    while (*at < bytes) {
        unsigned char c = (unsigned char)line[*at];
        if (c == '"') { (*at)++; return true; }
        if (c < 0x20U) return false;
        if (c == '\\') {
            (*at)++;
            if (*at >= bytes) return false;
            unsigned char escape = (unsigned char)line[(*at)++];
            if (escape == 'u') {
                if (*at + 4U > bytes) return false;
                for (unsigned i = 0; i < 4U; i++) {
                    unsigned char h = (unsigned char)line[*at + i];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F'))) return false;
                }
                *at += 4U;
            } else if (!(escape == '"' || escape == '\\' || escape == '/' ||
                         escape == 'b' || escape == 'f' || escape == 'n' ||
                         escape == 'r' || escape == 't')) return false;
            continue;
        }
        (*at)++;
    }
    return false;
}

static bool flat_skip_number(const char *line, size_t bytes, size_t *at)
{
    if (*at < bytes && line[*at] == '-') (*at)++;
    if (*at >= bytes) return false;
    if (line[*at] == '0') (*at)++;
    else if (line[*at] >= '1' && line[*at] <= '9')
        while (*at < bytes && line[*at] >= '0' && line[*at] <= '9') (*at)++;
    else return false;
    if (*at < bytes && line[*at] == '.') {
        (*at)++;
        size_t start = *at;
        while (*at < bytes && line[*at] >= '0' && line[*at] <= '9') (*at)++;
        if (*at == start) return false;
    }
    if (*at < bytes && (line[*at] == 'e' || line[*at] == 'E')) {
        (*at)++;
        if (*at < bytes && (line[*at] == '+' || line[*at] == '-')) (*at)++;
        size_t start = *at;
        while (*at < bytes && line[*at] >= '0' && line[*at] <= '9') (*at)++;
        if (*at == start) return false;
    }
    return true;
}

static bool flat_request_shape(const char *line, size_t bytes)
{
    size_t at = 0;
    unsigned fields = 0;
    flat_skip_ws(line, bytes, &at);
    if (at >= bytes || line[at++] != '{') return false;
    flat_skip_ws(line, bytes, &at);
    if (at < bytes && line[at] == '}') {
        at++;
    } else {
        for (;;) {
            flat_skip_ws(line, bytes, &at);
            if (!flat_skip_string(line, bytes, &at)) return false;
            flat_skip_ws(line, bytes, &at);
            if (at >= bytes || line[at++] != ':') return false;
            flat_skip_ws(line, bytes, &at);
            if (at < bytes && line[at] == '"') {
                if (!flat_skip_string(line, bytes, &at)) return false;
            } else if (!flat_skip_number(line, bytes, &at)) {
                return false; /* objects/arrays/booleans/null are not scalar fields here */
            }
            fields++;
            flat_skip_ws(line, bytes, &at);
            if (at < bytes && line[at] == ',') { at++; continue; }
            if (at < bytes && line[at] == '}') { at++; break; }
            return false;
        }
    }
    flat_skip_ws(line, bytes, &at);
    return at == bytes && fields == USB_KEYS;
}

static cJSON *strict_object(const char *line, size_t bytes)
{
    if (!line || !bytes || bytes > SUITE_TRANSLATION_USB_FRAME_MAX ||
        !utf8_valid((const unsigned char *)line, bytes) || memchr(line, 0, bytes) ||
        contains_nul_escape(line, bytes))
        return NULL;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(line, bytes, &end, false);
    if (!json) return NULL;
    while (end < line + bytes && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end++;
    if (end != line + bytes || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return NULL;
    }
    for (cJSON *a = json->child; a; a = a->next)
        for (cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && !strcmp(a->string, b->string)) {
                cJSON_Delete(json);
                return NULL;
            }
    return json;
}

static const char *text(cJSON *json, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsString(item) || !item->valuestring) return NULL;
    for (const unsigned char *s = (const unsigned char *)item->valuestring; *s; s++)
        if (*s < 0x20U || *s == 0x7fU) return NULL;
    return item->valuestring;
}

suite_translation_usb_parse_t suite_translation_usb_parse(
    const char *line, size_t bytes, suite_translation_usb_request_t *request)
{
    if (!request) return SUITE_TRANSLATION_USB_PARSE_IGNORE;
    memset(request, 0, sizeof(*request));
    if (!line || !bytes || bytes > SUITE_TRANSLATION_USB_FRAME_MAX)
        return SUITE_TRANSLATION_USB_PARSE_IGNORE;
    char fallback_id[33];
    bool fallback = raw_request_id(line, bytes, fallback_id);
    if (memchr(line, 0, bytes) || !utf8_valid((const unsigned char *)line, bytes) ||
        contains_nul_escape(line, bytes) || !flat_request_shape(line, bytes)) {
        if (!fallback) return SUITE_TRANSLATION_USB_PARSE_IGNORE;
        memcpy(request->request_id, fallback_id, sizeof(fallback_id));
        return SUITE_TRANSLATION_USB_PARSE_INVALID;
    }

    cJSON *json = strict_object(line, bytes);
    if (!json) {
        if (!fallback) return SUITE_TRANSLATION_USB_PARSE_IGNORE;
        memcpy(request->request_id, fallback_id, sizeof(fallback_id));
        return SUITE_TRANSLATION_USB_PARSE_INVALID;
    }

    /* Extract a trustworthy request ID for error replies. Duplicate keys were
       already rejected, so at most one exact request_id child exists. */
    const char *id = text(json, "request_id");
    if (!hex32(id)) {
        cJSON_Delete(json);
        if (!fallback) return SUITE_TRANSLATION_USB_PARSE_IGNORE;
        memcpy(request->request_id, fallback_id, sizeof(fallback_id));
        return SUITE_TRANSLATION_USB_PARSE_INVALID;
    }
    strcpy(request->request_id, id);

    unsigned seen = 0;
    for (cJSON *item = json->child; item; item = item->next) {
        const char *keys[USB_KEYS] = {"schema", "operation", "request_id", "origin", "token"};
        unsigned found = USB_KEYS;
        for (unsigned i = 0; i < USB_KEYS; i++)
            if (item->string && !strcmp(item->string, keys[i])) { found = i; break; }
        if (found == USB_KEYS || (seen & (1U << found))) {
            cJSON_Delete(json);
            return SUITE_TRANSLATION_USB_PARSE_INVALID;
        }
        seen |= 1U << found;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    const char *operation = text(json, "operation");
    const char *origin = text(json, "origin");
    const char *token = text(json, "token");
    bool valid = seen == (1U << USB_KEYS) - 1U && cJSON_IsNumber(schema) &&
        schema->valuedouble == 1.0 && operation && !strcmp(operation, "set_translation_config") &&
        suite_translation_config_origin_valid(origin) &&
        suite_translation_config_token_valid(token);
    if (valid) {
        strcpy(request->origin, origin);
        strcpy(request->token, token);
    }
    cJSON_Delete(json);
    return valid ? SUITE_TRANSLATION_USB_PARSE_REQUEST : SUITE_TRANSLATION_USB_PARSE_INVALID;
}

static uint32_t payload_tag(const char *origin, const char *token)
{
    uint32_t tag = 2166136261U;
    for (const char *s = origin; *s; s++) { tag ^= (unsigned char)*s; tag *= 16777619U; }
    tag ^= 0xffU; tag *= 16777619U;
    for (const char *s = token; *s; s++) { tag ^= (unsigned char)*s; tag *= 16777619U; }
    return tag;
}

void suite_translation_usb_session_reset(suite_translation_usb_session_t *session)
{
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->open = true;
    session->status = SUITE_TRANSLATION_USB_STATUS_INVALID;
}

suite_translation_usb_step_t suite_translation_usb_session_feed(
    suite_translation_usb_session_t *session, const char *line, size_t bytes,
    suite_translation_usb_request_t *request)
{
    if (!session || !session->open || !request) return SUITE_TRANSLATION_USB_STEP_NONE;
    suite_translation_usb_parse_t parsed = suite_translation_usb_parse(line, bytes, request);
    if (parsed == SUITE_TRANSLATION_USB_PARSE_IGNORE) return SUITE_TRANSLATION_USB_STEP_NONE;
    if (parsed == SUITE_TRANSLATION_USB_PARSE_INVALID) {
        strcpy(session->request_id, request->request_id);
        session->status = SUITE_TRANSLATION_USB_STATUS_INVALID;
        session->verified = false;
        return SUITE_TRANSLATION_USB_STEP_REPLY;
    }
    uint32_t tag = payload_tag(request->origin, request->token);
    if (session->write_consumed) {
        if (!strcmp(session->request_id, request->request_id) && session->payload_tag == tag) {
            return SUITE_TRANSLATION_USB_STEP_REPLY;
        }
        session->status = !strcmp(session->request_id, request->request_id) ?
            SUITE_TRANSLATION_USB_STATUS_INVALID : SUITE_TRANSLATION_USB_STATUS_BUSY;
        session->verified = false;
        return SUITE_TRANSLATION_USB_STEP_REPLY;
    }
    session->write_consumed = true;
    session->payload_tag = tag;
    strcpy(session->request_id, request->request_id);
    return SUITE_TRANSLATION_USB_STEP_APPLY;
}

void suite_translation_usb_session_complete(suite_translation_usb_session_t *session,
                                            suite_translation_usb_status_t status,
                                            bool verified)
{
    if (!session || !session->open) return;
    session->status = status;
    session->verified = verified;
}

const char *suite_translation_usb_status_name(suite_translation_usb_status_t status)
{
    switch (status) {
    case SUITE_TRANSLATION_USB_STATUS_OK: return "OK";
    case SUITE_TRANSLATION_USB_STATUS_INVALID: return "INVALID";
    case SUITE_TRANSLATION_USB_STATUS_BUSY: return "BUSY";
    case SUITE_TRANSLATION_USB_STATUS_STORAGE: return "STORAGE";
    case SUITE_TRANSLATION_USB_STATUS_UNVERIFIED: return "UNVERIFIED";
    default: return "INVALID";
    }
}

size_t suite_translation_usb_ack(char *out, size_t capacity, const char *request_id,
                                 suite_translation_usb_status_t status, bool verified)
{
    if (!out || !capacity || !hex32(request_id)) return 0;
    int n = snprintf(out, capacity, "{\"request_id\":\"%s\",\"status\":\"%s\",\"verified\":%s}\n",
                     request_id, suite_translation_usb_status_name(status),
                     verified ? "true" : "false");
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

static suite_translation_usb_status_t map_apply(suite_translation_apply_t applied, bool *verified)
{
    *verified = false;
    switch (applied) {
    case SUITE_TRANSLATION_APPLY_OK:
    case SUITE_TRANSLATION_APPLY_UNCHANGED:
        *verified = true;
        return SUITE_TRANSLATION_USB_STATUS_OK;
    case SUITE_TRANSLATION_APPLY_INVALID:
        return SUITE_TRANSLATION_USB_STATUS_INVALID;
    case SUITE_TRANSLATION_APPLY_STORAGE_NO_WRITE:
        return SUITE_TRANSLATION_USB_STATUS_STORAGE;
    default:
        return SUITE_TRANSLATION_USB_STATUS_UNVERIFIED;
    }
}

static bool send_ack(const suite_translation_usb_transport_t *transport,
                     suite_translation_usb_session_t *session)
{
    char ack[160];
    size_t length = suite_translation_usb_ack(ack, sizeof(ack), session->request_id,
                                              session->status, session->verified);
    if (!length) return false;
    int written = transport->write(transport->ctx, ack, length, 1000U);
    return written == (int)length;
}

suite_translation_usb_run_t suite_translation_usb_run(
    const suite_translation_usb_transport_t *transport,
    suite_translation_usb_apply_fn apply, void *apply_ctx,
    bool (*cancelled)(void *ctx), void *cancel_ctx, uint32_t window_ms)
{
    if (!transport || !transport->read || !transport->write || !transport->now_ms || !apply)
        return SUITE_TRANSLATION_USB_RUN_INVALID_ARGUMENT;
    if (!window_ms) window_ms = SUITE_TRANSLATION_USB_WINDOW_DEFAULT_MS;
    if (window_ms > SUITE_TRANSLATION_USB_WINDOW_MAX_MS)
        window_ms = SUITE_TRANSLATION_USB_WINDOW_MAX_MS;

    suite_translation_usb_framer_t framer;
    suite_translation_usb_session_t session;
    suite_translation_usb_request_t request;
    suite_translation_usb_framer_reset(&framer);
    suite_translation_usb_session_reset(&session);
    memset(&request, 0, sizeof(request));
    int64_t deadline = transport->now_ms(transport->ctx) + (int64_t)window_ms;
    char buffer[64];

    for (;;) {
        if (cancelled && cancelled(cancel_ctx)) {
            suite_translation_usb_session_reset(&session);
            session.open = false;
            memset(&request, 0, sizeof(request));
            return SUITE_TRANSLATION_USB_RUN_CANCELLED;
        }
        int64_t now = transport->now_ms(transport->ctx);
        if (now >= deadline) {
            session.open = false;
            memset(&request, 0, sizeof(request));
            return SUITE_TRANSLATION_USB_RUN_TIMEOUT;
        }
        uint32_t wait = (uint32_t)(deadline - now);
        if (wait > 100U) wait = 100U;
        int received = transport->read(transport->ctx, buffer, sizeof(buffer), wait);
        if (received < 0) {
            session.open = false;
            memset(&request, 0, sizeof(request));
            return SUITE_TRANSLATION_USB_RUN_IO_ERROR;
        }
        /* A read may have blocked past the deadline or a cancel may have been
           published while it waited. Close the window without any apply. */
        if (cancelled && cancelled(cancel_ctx)) {
            session.open = false;
            memset(&request, 0, sizeof(request));
            return SUITE_TRANSLATION_USB_RUN_CANCELLED;
        }
        if (transport->now_ms(transport->ctx) >= deadline) {
            session.open = false;
            memset(&request, 0, sizeof(request));
            return SUITE_TRANSLATION_USB_RUN_TIMEOUT;
        }
        for (int i = 0; i < received; i++) {
            suite_translation_usb_frame_t frame =
                suite_translation_usb_framer_push(&framer, buffer[i]);
            if (frame == SUITE_TRANSLATION_USB_FRAME_OVERSIZE) {
                memset(&request, 0, sizeof(request));
                continue;
            }
            if (frame != SUITE_TRANSLATION_USB_FRAME_READY) continue;
            memset(&request, 0, sizeof(request));
            suite_translation_usb_step_t step = suite_translation_usb_session_feed(
                &session, framer.line, framer.used, &request);
            framer.used = 0;
            if (step == SUITE_TRANSLATION_USB_STEP_NONE) continue;
            if (step == SUITE_TRANSLATION_USB_STEP_APPLY) {
                /* Establish the commit boundary: no cancellation and no expiry
                   may be observed between this check and the synchronous NVS
                   apply call. */
                if (cancelled && cancelled(cancel_ctx)) {
                    session.open = false;
                    memset(&request, 0, sizeof(request));
                    return SUITE_TRANSLATION_USB_RUN_CANCELLED;
                }
                if (transport->now_ms(transport->ctx) >= deadline) {
                    session.open = false;
                    memset(&request, 0, sizeof(request));
                    return SUITE_TRANSLATION_USB_RUN_TIMEOUT;
                }
                bool verified = false;
                suite_translation_usb_status_t status =
                    map_apply(apply(request.origin, request.token, apply_ctx), &verified);
                suite_translation_usb_session_complete(&session, status, verified);
            }
            bool acked = send_ack(transport, &session);
            memset(&request, 0, sizeof(request));
            if (!acked) {
                session.open = false;
                return SUITE_TRANSLATION_USB_RUN_IO_ERROR;
            }
            /* One bounded request/ID per window: an applied or rejected frame
               with a validated ID closes it. Untrusted noise (no ID) does not. */
            if (session.write_consumed || session.request_id[0]) {
                session.open = false;
                return SUITE_TRANSLATION_USB_RUN_COMPLETED;
            }
        }
    }
}
