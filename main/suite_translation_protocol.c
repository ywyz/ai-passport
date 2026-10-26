#include "suite_translation_protocol.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    unsigned depth;
    uint64_t keys[SUITE_TR_PROTOCOL_KEYS_MAX];
    unsigned key_count;
} parser_t;

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(uint8_t c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_value(uint8_t c)
{
    if (is_digit(c)) return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return (uint8_t)(c - 'A' + 10);
}

static bool is_error_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_';
}

/* Validate UTF-8 across the whole document. This rejects overlong forms,
 * surrogate code points, values above U+10FFFF, truncated sequences and raw
 * NUL. Escaped NUL is rejected separately while decoding strings. */
static suite_translation_protocol_error_t utf8_check(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i++];
        if (c == 0) return SUITE_TR_PROTOCOL_E_UTF8;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= n || (s[i] & 0xc0) != 0x80) return SUITE_TR_PROTOCOL_E_UTF8;
            i++;
            continue;
        }
        if (c >= 0xe0 && c <= 0xef) {
            if (i + 1 >= n) return SUITE_TR_PROTOCOL_E_UTF8;
            uint8_t c1 = s[i], c2 = s[i + 1];
            if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80)
                return SUITE_TR_PROTOCOL_E_UTF8;
            if (c == 0xe0 && c1 < 0xa0) return SUITE_TR_PROTOCOL_E_UTF8;
            if (c == 0xed && c1 >= 0xa0) return SUITE_TR_PROTOCOL_E_UTF8;
            i += 2;
            continue;
        }
        if (c >= 0xf0 && c <= 0xf4) {
            if (i + 2 >= n) return SUITE_TR_PROTOCOL_E_UTF8;
            uint8_t c1 = s[i], c2 = s[i + 1], c3 = s[i + 2];
            if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80 ||
                (c3 & 0xc0) != 0x80)
                return SUITE_TR_PROTOCOL_E_UTF8;
            if (c == 0xf0 && c1 < 0x90) return SUITE_TR_PROTOCOL_E_UTF8;
            if (c == 0xf4 && c1 > 0x8f) return SUITE_TR_PROTOCOL_E_UTF8;
            i += 3;
            continue;
        }
        return SUITE_TR_PROTOCOL_E_UTF8;
    }
    return SUITE_TR_PROTOCOL_OK;
}

static size_t utf8_length(uint8_t lead)
{
    if (lead < 0x80) return 1;
    if (lead < 0xe0) return 2;
    if (lead < 0xf0) return 3;
    return 4;
}

static size_t utf8_size(uint32_t cp)
{
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

static suite_translation_protocol_error_t utf8_put(uint32_t cp, char *out,
                                                   size_t cap, size_t *length)
{
    uint8_t bytes[4];
    size_t n = utf8_size(cp);
    if (cp < 0x80) {
        bytes[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (uint8_t)(0xc0 | (cp >> 6));
        bytes[1] = (uint8_t)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (uint8_t)(0xe0 | (cp >> 12));
        bytes[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        bytes[0] = (uint8_t)(0xf0 | (cp >> 18));
        bytes[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        bytes[3] = (uint8_t)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (out) {
        if (*length + n + 1U > cap) return SUITE_TR_PROTOCOL_E_LIMIT;
        memcpy(out + *length, bytes, n);
    }
    *length += n;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t scan_hex4(parser_t *p, uint32_t *value)
{
    if ((size_t)(p->end - p->p) < 4) return SUITE_TR_PROTOCOL_E_JSON;
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t c = *p->p++;
        if (!is_hex(c)) return SUITE_TR_PROTOCOL_E_JSON;
        v = (v << 4) | hex_value(c);
    }
    *value = v;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t scan_escape(parser_t *p, uint32_t *cp)
{
    if (p->p >= p->end) return SUITE_TR_PROTOCOL_E_JSON;
    uint8_t c = *p->p++;
    switch (c) {
    case '"': *cp = '"'; return SUITE_TR_PROTOCOL_OK;
    case '\\': *cp = '\\'; return SUITE_TR_PROTOCOL_OK;
    case '/': *cp = '/'; return SUITE_TR_PROTOCOL_OK;
    case 'b': *cp = '\b'; return SUITE_TR_PROTOCOL_OK;
    case 'f': *cp = '\f'; return SUITE_TR_PROTOCOL_OK;
    case 'n': *cp = '\n'; return SUITE_TR_PROTOCOL_OK;
    case 'r': *cp = '\r'; return SUITE_TR_PROTOCOL_OK;
    case 't': *cp = '\t'; return SUITE_TR_PROTOCOL_OK;
    case 'u': {
        uint32_t high;
        suite_translation_protocol_error_t error = scan_hex4(p, &high);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        if (high >= 0xd800 && high <= 0xdbff) {
            if ((size_t)(p->end - p->p) < 6 || p->p[0] != '\\' ||
                p->p[1] != 'u')
                return SUITE_TR_PROTOCOL_E_SURROGATE;
            p->p += 2;
            uint32_t low;
            error = scan_hex4(p, &low);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            if (low < 0xdc00 || low > 0xdfff)
                return SUITE_TR_PROTOCOL_E_SURROGATE;
            *cp = 0x10000U + ((high - 0xd800U) << 10) + (low - 0xdc00U);
        } else if (high >= 0xdc00 && high <= 0xdfff) {
            return SUITE_TR_PROTOCOL_E_SURROGATE;
        } else {
            *cp = high;
        }
        if (*cp == 0) return SUITE_TR_PROTOCOL_E_NUL;
        return SUITE_TR_PROTOCOL_OK;
    }
    default:
        return SUITE_TR_PROTOCOL_E_JSON;
    }
}

/*
 * Scan one JSON string. `text_field` enables the translation text limits.
 * `out` may be NULL to validate and count without retaining the value.
 */
static suite_translation_protocol_error_t scan_string(
    parser_t *p, char *out, size_t cap, size_t *out_length,
    uint32_t *out_scalars, bool text_field)
{
    if (p->p >= p->end || *p->p != '"') return SUITE_TR_PROTOCOL_E_JSON;
    p->p++;
    size_t length = 0;
    uint32_t scalars = 0;
    for (;;) {
        if (p->p >= p->end) return SUITE_TR_PROTOCOL_E_JSON;
        uint8_t c = *p->p++;
        if (c == '"') break;
        if (c == '\\') {
            uint32_t cp;
            suite_translation_protocol_error_t error = scan_escape(p, &cp);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            size_t n = utf8_size(cp);
            if (text_field) {
                if (length + n > TR_TEXT_MAX_BYTES)
                    return SUITE_TR_PROTOCOL_E_TEXT_BYTES;
                if (scalars + 1U > TR_TEXT_MAX_SCALARS)
                    return SUITE_TR_PROTOCOL_E_TEXT_SCALARS;
            }
            error = utf8_put(cp, out, cap, &length);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            scalars++;
            continue;
        }
        if (c < 0x20) return SUITE_TR_PROTOCOL_E_JSON;
        if (c < 0x80) {
            if (text_field) {
                if (length + 1U > TR_TEXT_MAX_BYTES)
                    return SUITE_TR_PROTOCOL_E_TEXT_BYTES;
                if (scalars + 1U > TR_TEXT_MAX_SCALARS)
                    return SUITE_TR_PROTOCOL_E_TEXT_SCALARS;
            }
            if (out) {
                if (length + 2U > cap) return SUITE_TR_PROTOCOL_E_LIMIT;
                out[length] = (char)c;
            }
            length++;
            scalars++;
            continue;
        }
        size_t n = utf8_length(c);
        if ((size_t)(p->end - p->p) < n - 1U)
            return SUITE_TR_PROTOCOL_E_UTF8;
        if (text_field) {
            if (length + n > TR_TEXT_MAX_BYTES)
                return SUITE_TR_PROTOCOL_E_TEXT_BYTES;
            if (scalars + 1U > TR_TEXT_MAX_SCALARS)
                return SUITE_TR_PROTOCOL_E_TEXT_SCALARS;
        }
        if (out) {
            if (length + n + 1U > cap) return SUITE_TR_PROTOCOL_E_LIMIT;
            out[length] = (char)c;
            memcpy(out + length + 1U, p->p, n - 1U);
        }
        p->p += n - 1U;
        length += n;
        scalars++;
    }
    if (text_field) {
        if (length == 0) return SUITE_TR_PROTOCOL_E_TEXT;
        if (length > TR_TEXT_MAX_BYTES) return SUITE_TR_PROTOCOL_E_TEXT_BYTES;
        if (scalars > TR_TEXT_MAX_SCALARS)
            return SUITE_TR_PROTOCOL_E_TEXT_SCALARS;
    }
    if (out) {
        if (length + 1U > cap) return SUITE_TR_PROTOCOL_E_LIMIT;
        out[length] = 0;
    }
    if (out_length) *out_length = length;
    if (out_scalars) *out_scalars = scalars;
    return SUITE_TR_PROTOCOL_OK;
}

static void skip_ws(parser_t *p)
{
    while (p->p < p->end) {
        uint8_t c = *p->p;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->p++;
    }
}

static bool consume(parser_t *p, uint8_t c)
{
    if (p->p < p->end && *p->p == c) {
        p->p++;
        return true;
    }
    return false;
}

static suite_translation_protocol_error_t match_literal(parser_t *p,
                                                        const char *text)
{
    size_t n = strlen(text);
    if ((size_t)(p->end - p->p) < n || memcmp(p->p, text, n) != 0)
        return SUITE_TR_PROTOCOL_E_TYPE;
    p->p += n;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t parse_uint(parser_t *p, uint64_t *out)
{
    skip_ws(p);
    if (p->p >= p->end) return SUITE_TR_PROTOCOL_E_JSON;
    if (*p->p == '-') return SUITE_TR_PROTOCOL_E_NUMBER;
    if (!is_digit(*p->p)) return SUITE_TR_PROTOCOL_E_TYPE;
    uint64_t value = 0;
    if (*p->p == '0') {
        p->p++;
        if (p->p < p->end && is_digit(*p->p))
            return SUITE_TR_PROTOCOL_E_NUMBER;
    } else {
        while (p->p < p->end && is_digit(*p->p)) {
            uint8_t digit = (uint8_t)(*p->p++ - '0');
            if (value > (UINT64_MAX - digit) / 10U)
                return SUITE_TR_PROTOCOL_E_OVERFLOW;
            value = value * 10U + digit;
        }
    }
    if (p->p < p->end &&
        (*p->p == '.' || *p->p == 'e' || *p->p == 'E'))
        return SUITE_TR_PROTOCOL_E_NUMBER;
    *out = value;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t skip_number(parser_t *p)
{
    if (p->p < p->end && *p->p == '-') p->p++;
    if (p->p >= p->end || !is_digit(*p->p))
        return SUITE_TR_PROTOCOL_E_JSON;
    if (*p->p == '0') {
        p->p++;
        if (p->p < p->end && is_digit(*p->p))
            return SUITE_TR_PROTOCOL_E_NUMBER;
    } else {
        while (p->p < p->end && is_digit(*p->p)) p->p++;
    }
    if (p->p < p->end && *p->p == '.') {
        p->p++;
        if (p->p >= p->end || !is_digit(*p->p))
            return SUITE_TR_PROTOCOL_E_NUMBER;
        while (p->p < p->end && is_digit(*p->p)) p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-')) p->p++;
        if (p->p >= p->end || !is_digit(*p->p))
            return SUITE_TR_PROTOCOL_E_NUMBER;
        while (p->p < p->end && is_digit(*p->p)) p->p++;
    }
    return SUITE_TR_PROTOCOL_OK;
}

static uint64_t key_hash(const char *key, size_t length)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static suite_translation_protocol_error_t key_seen(parser_t *p, unsigned base,
                                                    const char *key,
                                                    size_t length)
{
    uint64_t hash = key_hash(key, length);
    for (unsigned i = base; i < p->key_count; i++) {
        if (p->keys[i] == hash) return SUITE_TR_PROTOCOL_E_DUPLICATE;
    }
    if (p->key_count >= SUITE_TR_PROTOCOL_KEYS_MAX)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    p->keys[p->key_count++] = hash;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t skip_value(parser_t *p);

static suite_translation_protocol_error_t skip_object(parser_t *p)
{
    if (p->depth >= SUITE_TR_PROTOCOL_DEPTH_MAX)
        return SUITE_TR_PROTOCOL_E_DEPTH;
    p->depth++;
    skip_ws(p);
    if (!consume(p, '{')) return SUITE_TR_PROTOCOL_E_JSON;
    skip_ws(p);
    unsigned base = p->key_count;
    if (consume(p, '}')) {
        p->key_count = base;
        p->depth--;
        return SUITE_TR_PROTOCOL_OK;
    }
    for (;;) {
        char key[SUITE_TR_PROTOCOL_KEYS_MAX + 1U];
        size_t key_length;
        suite_translation_protocol_error_t error =
            scan_string(p, key, sizeof(key), &key_length, NULL, false);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        error = key_seen(p, base, key, key_length);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        skip_ws(p);
        if (!consume(p, ':')) return SUITE_TR_PROTOCOL_E_JSON;
        error = skip_value(p);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        skip_ws(p);
        if (consume(p, '}')) break;
        if (!consume(p, ',')) return SUITE_TR_PROTOCOL_E_JSON;
        skip_ws(p);
    }
    p->key_count = base;
    p->depth--;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t skip_array(parser_t *p)
{
    if (p->depth >= SUITE_TR_PROTOCOL_DEPTH_MAX)
        return SUITE_TR_PROTOCOL_E_DEPTH;
    p->depth++;
    skip_ws(p);
    if (!consume(p, '[')) return SUITE_TR_PROTOCOL_E_JSON;
    skip_ws(p);
    if (consume(p, ']')) {
        p->depth--;
        return SUITE_TR_PROTOCOL_OK;
    }
    for (;;) {
        suite_translation_protocol_error_t error = skip_value(p);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        skip_ws(p);
        if (consume(p, ']')) break;
        if (!consume(p, ',')) return SUITE_TR_PROTOCOL_E_JSON;
        skip_ws(p);
    }
    p->depth--;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t skip_value(parser_t *p)
{
    skip_ws(p);
    if (p->p >= p->end) return SUITE_TR_PROTOCOL_E_JSON;
    uint8_t c = *p->p;
    if (c == '{') return skip_object(p);
    if (c == '[') return skip_array(p);
    if (c == '"') return scan_string(p, NULL, 0, NULL, NULL, false);
    if (c == 't') return match_literal(p, "true");
    if (c == 'f') return match_literal(p, "false");
    if (c == 'n') return match_literal(p, "null");
    if (c == '-' || is_digit(c)) return skip_number(p);
    return SUITE_TR_PROTOCOL_E_JSON;
}

static suite_translation_protocol_error_t parse_bool(parser_t *p, bool *value)
{
    skip_ws(p);
    if (p->p < p->end && *p->p == 't') {
        suite_translation_protocol_error_t error = match_literal(p, "true");
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        *value = true;
        return SUITE_TR_PROTOCOL_OK;
    }
    if (p->p < p->end && *p->p == 'f') {
        suite_translation_protocol_error_t error = match_literal(p, "false");
        if (error != SUITE_TR_PROTOCOL_OK) return error;
        *value = false;
        return SUITE_TR_PROTOCOL_OK;
    }
    return SUITE_TR_PROTOCOL_E_TYPE;
}

static suite_translation_protocol_error_t parse_null(parser_t *p)
{
    skip_ws(p);
    if (p->p < p->end && *p->p == 'n')
        return match_literal(p, "null");
    return SUITE_TR_PROTOCOL_E_TYPE;
}

static bool direction_valid(suite_translation_protocol_direction_t direction)
{
    return direction == SUITE_TR_PROTOCOL_DIRECTION_EN_ZH ||
        direction == SUITE_TR_PROTOCOL_DIRECTION_ZH_EN;
}

static suite_translation_protocol_state_t state_from_name(const char *name,
                                                          size_t length)
{
    if (length == 8 && !memcmp(name, "ADMITTED", 8))
        return SUITE_TR_PROTOCOL_STATE_ADMITTED;
    if (length == 7 && !memcmp(name, "RUNNING", 7))
        return SUITE_TR_PROTOCOL_STATE_RUNNING;
    if (length == 4 && !memcmp(name, "DONE", 4))
        return SUITE_TR_PROTOCOL_STATE_DONE;
    if (length == 6 && !memcmp(name, "FAILED", 6))
        return SUITE_TR_PROTOCOL_STATE_FAILED;
    if (length == 9 && !memcmp(name, "CANCELLED", 9))
        return SUITE_TR_PROTOCOL_STATE_CANCELLED;
    if (length == 7 && !memcmp(name, "EXPIRED", 7))
        return SUITE_TR_PROTOCOL_STATE_EXPIRED;
    if (length == 7 && !memcmp(name, "UNKNOWN", 7))
        return SUITE_TR_PROTOCOL_STATE_REMOTE_UNKNOWN;
    if (length == 9 && !memcmp(name, "TEXT_LOST", 9))
        return SUITE_TR_PROTOCOL_STATE_TEXT_LOST;
    return SUITE_TR_PROTOCOL_STATE_UNKNOWN;
}

static bool error_code_valid(const char *code, size_t length)
{
    if (length == 0 || length > SUITE_TR_PROTOCOL_ERROR_CODE_MAX) return false;
    if (code[0] < 'A' || code[0] > 'Z') return false;
    for (size_t i = 1; i < length; i++) {
        if (!is_error_char((uint8_t)code[i])) return false;
    }
    return true;
}

static suite_translation_protocol_error_t parse_identity(
    const suite_translation_protocol_view_t *view,
    const suite_translation_protocol_identity_t *expected, bool cancel)
{
    if (!expected || !expected->operation_id || !expected->job_id)
        return SUITE_TR_PROTOCOL_E_ARGUMENT;
    if (strcmp(view->operation_id, expected->operation_id))
        return SUITE_TR_PROTOCOL_E_IDENTITY;
    if (strcmp(view->job_id, expected->job_id))
        return SUITE_TR_PROTOCOL_E_IDENTITY;
    if (view->direction != expected->direction)
        return SUITE_TR_PROTOCOL_E_IDENTITY;
    if (view->audio_bytes != expected->audio_bytes)
        return SUITE_TR_PROTOCOL_E_IDENTITY;
    if (cancel) {
        if (view->state != SUITE_TR_PROTOCOL_STATE_CANCELLED)
            return SUITE_TR_PROTOCOL_E_STATE;
        uint32_t next = expected->generation;
        if (next != UINT32_MAX) next++;
        if (view->generation != expected->generation &&
            view->generation != next)
            return SUITE_TR_PROTOCOL_E_IDENTITY;
    } else if (view->generation != expected->generation) {
        return SUITE_TR_PROTOCOL_E_IDENTITY;
    }
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t parse_view_document(
    const uint8_t *json, size_t length,
    const suite_translation_protocol_identity_t *expected, bool admission,
    bool cancel, suite_translation_protocol_view_t *view)
{
    if (!json || !view || length == 0 ||
        length > SUITE_TR_PROTOCOL_JSON_MAX)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    if (view->source && view->source_cap < SUITE_TR_PROTOCOL_TEXT_CAP)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    if (view->target && view->target_cap < SUITE_TR_PROTOCOL_TEXT_CAP)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    char *source = view->source;
    size_t source_cap = view->source_cap;
    char *target = view->target;
    size_t target_cap = view->target_cap;
    memset(view, 0, sizeof(*view));
    view->source = source;
    view->source_cap = source_cap;
    view->target = target;
    view->target_cap = target_cap;

    suite_translation_protocol_error_t error = utf8_check(json, length);
    if (error != SUITE_TR_PROTOCOL_OK) return error;

    parser_t parser = {.p = json, .end = json + length};
    skip_ws(&parser);
    if (parser.depth >= SUITE_TR_PROTOCOL_DEPTH_MAX)
        return SUITE_TR_PROTOCOL_E_DEPTH;
    parser.depth++;
    if (!consume(&parser, '{')) return SUITE_TR_PROTOCOL_E_JSON;
    skip_ws(&parser);

    bool has_schema = false, has_op = false, has_job = false;
    bool has_state = false, has_source_lang = false, has_target_lang = false;
    bool has_generation = false, has_audio = false;
    bool has_source_bytes = false, has_target_bytes = false;
    bool has_updated = false, has_error = false;
    char source_lang[8] = {0}, target_lang[8] = {0};
    size_t source_lang_length = 0, target_lang_length = 0;
    unsigned base = parser.key_count;

    if (!consume(&parser, '}')) {
        for (;;) {
            char key[SUITE_TR_PROTOCOL_KEYS_MAX + 1U];
            size_t key_length;
            error = scan_string(&parser, key, sizeof(key), &key_length, NULL,
                                false);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            error = key_seen(&parser, base, key, key_length);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            skip_ws(&parser);
            if (!consume(&parser, ':')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);

            if (!strcmp(key, "schema")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value != 1) return SUITE_TR_PROTOCOL_E_SCHEMA;
                has_schema = true;
            } else if (!strcmp(key, "operation_id")) {
                error = scan_string(&parser, view->operation_id,
                                    sizeof(view->operation_id), NULL, NULL,
                                    false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (!suite_translation_protocol_id_valid(
                        view->operation_id))
                    return SUITE_TR_PROTOCOL_E_ID;
                has_op = true;
            } else if (!strcmp(key, "job_id")) {
                error = scan_string(&parser, view->job_id,
                                    sizeof(view->job_id), NULL, NULL, false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (!suite_translation_protocol_id_valid(view->job_id))
                    return SUITE_TR_PROTOCOL_E_ID;
                has_job = true;
            } else if (!strcmp(key, "state")) {
                char state[16];
                size_t state_length;
                error = scan_string(&parser, state, sizeof(state),
                                    &state_length, NULL, false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                view->state = state_from_name(state, state_length);
                if (view->state == SUITE_TR_PROTOCOL_STATE_UNKNOWN)
                    return SUITE_TR_PROTOCOL_E_STATE;
                has_state = true;
            } else if (!strcmp(key, "source_lang")) {
                error = scan_string(&parser, source_lang, sizeof(source_lang),
                                    &source_lang_length, NULL, false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                has_source_lang = true;
            } else if (!strcmp(key, "target_lang")) {
                error = scan_string(&parser, target_lang, sizeof(target_lang),
                                    &target_lang_length, NULL, false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                has_target_lang = true;
            } else if (!strcmp(key, "generation")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value == 0 || value > UINT32_MAX)
                    return SUITE_TR_PROTOCOL_E_VALUE;
                view->generation = (uint32_t)value;
                has_generation = true;
            } else if (!strcmp(key, "audio_bytes")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value < SUITE_TR_PROTOCOL_AUDIO_MIN ||
                    value > TR_WAV_MAX_BYTES)
                    return SUITE_TR_PROTOCOL_E_VALUE;
                view->audio_bytes = (uint32_t)value;
                has_audio = true;
            } else if (!strcmp(key, "source_bytes")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value > TR_TEXT_MAX_BYTES)
                    return SUITE_TR_PROTOCOL_E_VALUE;
                view->source_bytes = (uint32_t)value;
                has_source_bytes = true;
            } else if (!strcmp(key, "target_bytes")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value > TR_TEXT_MAX_BYTES)
                    return SUITE_TR_PROTOCOL_E_VALUE;
                view->target_bytes = (uint32_t)value;
                has_target_bytes = true;
            } else if (!strcmp(key, "updated_at")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value > INT64_MAX) return SUITE_TR_PROTOCOL_E_OVERFLOW;
                view->updated_at = (int64_t)value;
                has_updated = true;
            } else if (!strcmp(key, "error")) {
                skip_ws(&parser);
                if (parser.p < parser.end && *parser.p == 'n') {
                    error = parse_null(&parser);
                    if (error != SUITE_TR_PROTOCOL_OK) return error;
                    view->has_error = false;
                } else {
                    error = scan_string(&parser, view->error,
                                        sizeof(view->error), NULL, NULL,
                                        false);
                    if (error != SUITE_TR_PROTOCOL_OK) return error;
                    if (!error_code_valid(view->error, strlen(view->error)))
                        return SUITE_TR_PROTOCOL_E_VALUE;
                    view->has_error = true;
                }
                has_error = true;
            } else if (!strcmp(key, "text_lost")) {
                error = parse_bool(&parser, &view->text_lost);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
            } else if (!strcmp(key, "source_text")) {
                error = scan_string(&parser, view->source, view->source_cap,
                                    &view->source_length, NULL, true);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                view->has_source = true;
            } else if (!strcmp(key, "target_text")) {
                error = scan_string(&parser, view->target, view->target_cap,
                                    &view->target_length, NULL, true);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                view->has_target = true;
            } else {
                error = skip_value(&parser);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
            }

            skip_ws(&parser);
            if (consume(&parser, '}')) break;
            if (!consume(&parser, ',')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);
        }
    }
    parser.key_count = base;
    parser.depth--;
    skip_ws(&parser);
    if (parser.p != parser.end) return SUITE_TR_PROTOCOL_E_TRAILING;

    if (!has_schema || !has_op || !has_job || !has_state ||
        !has_source_lang || !has_target_lang || !has_generation ||
        !has_audio || !has_source_bytes || !has_target_bytes ||
        !has_updated || !has_error)
        return SUITE_TR_PROTOCOL_E_MISSING;

    if (source_lang_length == 2 && !memcmp(source_lang, "en", 2) &&
        target_lang_length == 5 && !memcmp(target_lang, "zh-CN", 5)) {
        view->direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH;
    } else if (source_lang_length == 5 &&
               !memcmp(source_lang, "zh-CN", 5) &&
               target_lang_length == 2 && !memcmp(target_lang, "en", 2)) {
        view->direction = SUITE_TR_PROTOCOL_DIRECTION_ZH_EN;
    } else {
        return SUITE_TR_PROTOCOL_E_DIRECTION;
    }
    if (!direction_valid(view->direction))
        return SUITE_TR_PROTOCOL_E_DIRECTION;

    if (view->state == SUITE_TR_PROTOCOL_STATE_DONE) {
        if (!view->has_source || !view->has_target)
            return SUITE_TR_PROTOCOL_E_TEXT;
        if (view->source_length != view->source_bytes ||
            view->target_length != view->target_bytes)
            return SUITE_TR_PROTOCOL_E_VALUE;
        if (view->text_lost || view->has_error)
            return SUITE_TR_PROTOCOL_E_STATE;
    } else if (view->state == SUITE_TR_PROTOCOL_STATE_TEXT_LOST) {
        if (!view->text_lost || view->has_target)
            return SUITE_TR_PROTOCOL_E_STATE;
        if (view->has_source &&
            view->source_length != view->source_bytes)
            return SUITE_TR_PROTOCOL_E_VALUE;
    } else if (view->state == SUITE_TR_PROTOCOL_STATE_FAILED) {
        if (!view->has_error || view->has_target || view->text_lost)
            return SUITE_TR_PROTOCOL_E_STATE;
        if (view->has_source &&
            view->source_length != view->source_bytes)
            return SUITE_TR_PROTOCOL_E_VALUE;
    } else {
        if (view->has_source || view->has_target || view->text_lost)
            return SUITE_TR_PROTOCOL_E_STATE;
    }

    if (admission) {
        if (!expected || !expected->operation_id)
            return SUITE_TR_PROTOCOL_E_ARGUMENT;
        if (strcmp(view->operation_id, expected->operation_id))
            return SUITE_TR_PROTOCOL_E_IDENTITY;
        if (view->direction != expected->direction)
            return SUITE_TR_PROTOCOL_E_IDENTITY;
        if (view->audio_bytes != expected->audio_bytes)
            return SUITE_TR_PROTOCOL_E_IDENTITY;
        if (!strcmp(view->job_id, view->operation_id))
            return SUITE_TR_PROTOCOL_E_IDENTITY;
    } else {
        error = parse_identity(view, expected, cancel);
        if (error != SUITE_TR_PROTOCOL_OK) return error;
    }
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t parse_ready_document(
    const uint8_t *json, size_t length,
    suite_translation_protocol_ready_t *ready)
{
    if (!json || !ready || length == 0 ||
        length > SUITE_TR_PROTOCOL_JSON_MAX)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    memset(ready, 0, sizeof(*ready));
    suite_translation_protocol_error_t error = utf8_check(json, length);
    if (error != SUITE_TR_PROTOCOL_OK) return error;

    parser_t parser = {.p = json, .end = json + length};
    skip_ws(&parser);
    parser.depth++;
    if (!consume(&parser, '{')) return SUITE_TR_PROTOCOL_E_JSON;
    skip_ws(&parser);
    bool has_schema = false, has_ready = false, has_provider = false;
    unsigned base = parser.key_count;
    if (!consume(&parser, '}')) {
        for (;;) {
            char key[SUITE_TR_PROTOCOL_KEYS_MAX + 1U];
            size_t key_length;
            error = scan_string(&parser, key, sizeof(key), &key_length, NULL,
                                false);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            error = key_seen(&parser, base, key, key_length);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            skip_ws(&parser);
            if (!consume(&parser, ':')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);
            if (!strcmp(key, "schema")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value != 1) return SUITE_TR_PROTOCOL_E_SCHEMA;
                has_schema = true;
            } else if (!strcmp(key, "ready")) {
                error = parse_bool(&parser, &ready->ready);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                has_ready = true;
            } else if (!strcmp(key, "provider")) {
                size_t provider_length;
                error = scan_string(&parser, ready->provider,
                                    sizeof(ready->provider),
                                    &provider_length, NULL, false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (provider_length == 0 ||
                    provider_length > SUITE_TR_PROTOCOL_PROVIDER_MAX)
                    return SUITE_TR_PROTOCOL_E_PROVIDER;
                for (size_t i = 0; i < provider_length; i++) {
                    uint8_t c = (uint8_t)ready->provider[i];
                    if (!((c >= 'a' && c <= 'z') || is_digit(c) ||
                          c == '.' || c == '_' || c == '-'))
                        return SUITE_TR_PROTOCOL_E_PROVIDER;
                }
                has_provider = true;
            } else {
                error = skip_value(&parser);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
            }
            skip_ws(&parser);
            if (consume(&parser, '}')) break;
            if (!consume(&parser, ',')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);
        }
    }
    parser.key_count = base;
    parser.depth--;
    skip_ws(&parser);
    if (parser.p != parser.end) return SUITE_TR_PROTOCOL_E_TRAILING;
    if (!has_schema || !has_ready || !has_provider)
        return SUITE_TR_PROTOCOL_E_MISSING;
    if (ready->ready && strcmp(ready->provider, "mimo"))
        return SUITE_TR_PROTOCOL_E_PROVIDER;
    return SUITE_TR_PROTOCOL_OK;
}

static suite_translation_protocol_error_t parse_error_document(
    const uint8_t *json, size_t length, char *code, size_t code_cap)
{
    if (!json || !code || code_cap == 0 || length == 0 ||
        length > SUITE_TR_PROTOCOL_JSON_MAX)
        return SUITE_TR_PROTOCOL_E_LIMIT;
    suite_translation_protocol_error_t error = utf8_check(json, length);
    if (error != SUITE_TR_PROTOCOL_OK) return error;

    parser_t parser = {.p = json, .end = json + length};
    skip_ws(&parser);
    parser.depth++;
    if (!consume(&parser, '{')) return SUITE_TR_PROTOCOL_E_JSON;
    skip_ws(&parser);
    bool has_error = false;
    unsigned base = parser.key_count;
    if (!consume(&parser, '}')) {
        for (;;) {
            char key[SUITE_TR_PROTOCOL_KEYS_MAX + 1U];
            size_t key_length;
            error = scan_string(&parser, key, sizeof(key), &key_length, NULL,
                                false);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            error = key_seen(&parser, base, key, key_length);
            if (error != SUITE_TR_PROTOCOL_OK) return error;
            skip_ws(&parser);
            if (!consume(&parser, ':')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);
            if (!strcmp(key, "schema")) {
                uint64_t value;
                error = parse_uint(&parser, &value);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (value != 1) return SUITE_TR_PROTOCOL_E_SCHEMA;
            } else if (!strcmp(key, "error")) {
                error = scan_string(&parser, code, code_cap, NULL, NULL,
                                    false);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
                if (!error_code_valid(code, strlen(code)))
                    return SUITE_TR_PROTOCOL_E_VALUE;
                has_error = true;
            } else {
                error = skip_value(&parser);
                if (error != SUITE_TR_PROTOCOL_OK) return error;
            }
            skip_ws(&parser);
            if (consume(&parser, '}')) break;
            if (!consume(&parser, ',')) return SUITE_TR_PROTOCOL_E_JSON;
            skip_ws(&parser);
        }
    }
    parser.key_count = base;
    parser.depth--;
    skip_ws(&parser);
    if (parser.p != parser.end) return SUITE_TR_PROTOCOL_E_TRAILING;
    if (!has_error) return SUITE_TR_PROTOCOL_E_MISSING;
    return SUITE_TR_PROTOCOL_OK;
}

void suite_translation_protocol_view_init(suite_translation_protocol_view_t *view,
                                          char *source, size_t source_cap,
                                          char *target, size_t target_cap)
{
    if (!view) return;
    memset(view, 0, sizeof(*view));
    view->source = source;
    view->source_cap = source_cap;
    view->target = target;
    view->target_cap = target_cap;
}

suite_translation_protocol_error_t suite_translation_protocol_parse_admission(
    const uint8_t *json, size_t length, const char *operation_id,
    suite_translation_protocol_direction_t direction, uint32_t audio_bytes,
    suite_translation_protocol_view_t *view)
{
    if (!operation_id || !suite_translation_protocol_id_valid(operation_id) ||
        !direction_valid(direction) ||
        audio_bytes < SUITE_TR_PROTOCOL_AUDIO_MIN ||
        audio_bytes > TR_WAV_MAX_BYTES)
        return SUITE_TR_PROTOCOL_E_ARGUMENT;
    suite_translation_protocol_identity_t expected = {
        .operation_id = operation_id,
        .job_id = NULL,
        .generation = 0,
        .direction = direction,
        .audio_bytes = audio_bytes,
    };
    return parse_view_document(json, length, &expected, true, false, view);
}

suite_translation_protocol_error_t suite_translation_protocol_parse_view(
    const uint8_t *json, size_t length,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view)
{
    if (!expected || !expected->operation_id || !expected->job_id ||
        !suite_translation_protocol_id_valid(expected->operation_id) ||
        !suite_translation_protocol_id_valid(expected->job_id) ||
        !direction_valid(expected->direction) || expected->generation == 0 ||
        expected->audio_bytes < SUITE_TR_PROTOCOL_AUDIO_MIN ||
        expected->audio_bytes > TR_WAV_MAX_BYTES)
        return SUITE_TR_PROTOCOL_E_ARGUMENT;
    return parse_view_document(json, length, expected, false, false, view);
}

suite_translation_protocol_error_t suite_translation_protocol_parse_cancel_view(
    const uint8_t *json, size_t length,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view)
{
    if (!expected || !expected->operation_id || !expected->job_id ||
        !suite_translation_protocol_id_valid(expected->operation_id) ||
        !suite_translation_protocol_id_valid(expected->job_id) ||
        !direction_valid(expected->direction) || expected->generation == 0 ||
        expected->audio_bytes < SUITE_TR_PROTOCOL_AUDIO_MIN ||
        expected->audio_bytes > TR_WAV_MAX_BYTES)
        return SUITE_TR_PROTOCOL_E_ARGUMENT;
    return parse_view_document(json, length, expected, false, true, view);
}

suite_translation_protocol_error_t suite_translation_protocol_parse_ready(
    const uint8_t *json, size_t length,
    suite_translation_protocol_ready_t *ready)
{
    return parse_ready_document(json, length, ready);
}

suite_translation_protocol_error_t suite_translation_protocol_parse_error(
    const uint8_t *json, size_t length, char *code, size_t code_cap)
{
    return parse_error_document(json, length, code, code_cap);
}

suite_translation_protocol_error_t suite_translation_protocol_build_admission(
    char *out, size_t cap, const char *operation_id,
    suite_translation_protocol_direction_t direction, const char *audio_sha256,
    uint32_t audio_bytes, size_t *out_length)
{
    if (!out || cap == 0 || !operation_id || !audio_sha256 ||
        !suite_translation_protocol_id_valid(operation_id) ||
        !suite_translation_protocol_sha256_valid(audio_sha256) ||
        !direction_valid(direction) ||
        audio_bytes < SUITE_TR_PROTOCOL_AUDIO_MIN ||
        audio_bytes > TR_WAV_MAX_BYTES)
        return SUITE_TR_PROTOCOL_E_ARGUMENT;
    int n = snprintf(out, cap,
        "{\"schema\":1,\"operation_id\":\"%s\",\"source_lang\":\"%s\","
        "\"target_lang\":\"%s\",\"format\":\"wav\",\"audio_sha256\":\"%s\","
        "\"audio_bytes\":%lu}",
        operation_id, suite_translation_protocol_direction_source(direction),
        suite_translation_protocol_direction_target(direction),
        audio_sha256, (unsigned long)audio_bytes);
    if (n < 0 || (size_t)n >= cap) return SUITE_TR_PROTOCOL_E_LIMIT;
    if (out_length) *out_length = (size_t)n;
    return SUITE_TR_PROTOCOL_OK;
}

bool suite_translation_protocol_id_valid(const char *id)
{
    if (!id || strlen(id) != 32) return false;
    for (size_t i = 0; i < 32; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool suite_translation_protocol_sha256_valid(const char *sha256)
{
    if (!sha256 || strlen(sha256) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

const char *suite_translation_protocol_error_name(
    suite_translation_protocol_error_t error)
{
    switch (error) {
    case SUITE_TR_PROTOCOL_OK: return "OK";
    case SUITE_TR_PROTOCOL_E_ARGUMENT: return "ARGUMENT";
    case SUITE_TR_PROTOCOL_E_JSON: return "JSON";
    case SUITE_TR_PROTOCOL_E_UTF8: return "UTF8";
    case SUITE_TR_PROTOCOL_E_DUPLICATE: return "DUPLICATE";
    case SUITE_TR_PROTOCOL_E_NUL: return "NUL";
    case SUITE_TR_PROTOCOL_E_SURROGATE: return "SURROGATE";
    case SUITE_TR_PROTOCOL_E_NUMBER: return "NUMBER";
    case SUITE_TR_PROTOCOL_E_VALUE: return "VALUE";
    case SUITE_TR_PROTOCOL_E_OVERFLOW: return "OVERFLOW";
    case SUITE_TR_PROTOCOL_E_LIMIT: return "LIMIT";
    case SUITE_TR_PROTOCOL_E_DEPTH: return "DEPTH";
    case SUITE_TR_PROTOCOL_E_TYPE: return "TYPE";
    case SUITE_TR_PROTOCOL_E_MISSING: return "MISSING";
    case SUITE_TR_PROTOCOL_E_SCHEMA: return "SCHEMA";
    case SUITE_TR_PROTOCOL_E_ID: return "ID";
    case SUITE_TR_PROTOCOL_E_SHA256: return "SHA256";
    case SUITE_TR_PROTOCOL_E_DIRECTION: return "DIRECTION";
    case SUITE_TR_PROTOCOL_E_STATE: return "STATE";
    case SUITE_TR_PROTOCOL_E_TEXT: return "TEXT";
    case SUITE_TR_PROTOCOL_E_TEXT_BYTES: return "TEXT_BYTES";
    case SUITE_TR_PROTOCOL_E_TEXT_SCALARS: return "TEXT_SCALARS";
    case SUITE_TR_PROTOCOL_E_IDENTITY: return "IDENTITY";
    case SUITE_TR_PROTOCOL_E_PROVIDER: return "PROVIDER";
    case SUITE_TR_PROTOCOL_E_TRAILING: return "TRAILING";
    default: return "UNKNOWN";
    }
}

const char *suite_translation_protocol_state_name(
    suite_translation_protocol_state_t state)
{
    switch (state) {
    case SUITE_TR_PROTOCOL_STATE_ADMITTED: return "ADMITTED";
    case SUITE_TR_PROTOCOL_STATE_RUNNING: return "RUNNING";
    case SUITE_TR_PROTOCOL_STATE_DONE: return "DONE";
    case SUITE_TR_PROTOCOL_STATE_FAILED: return "FAILED";
    case SUITE_TR_PROTOCOL_STATE_CANCELLED: return "CANCELLED";
    case SUITE_TR_PROTOCOL_STATE_EXPIRED: return "EXPIRED";
    case SUITE_TR_PROTOCOL_STATE_REMOTE_UNKNOWN: return "UNKNOWN";
    case SUITE_TR_PROTOCOL_STATE_TEXT_LOST: return "TEXT_LOST";
    default: return "UNKNOWN";
    }
}

const char *suite_translation_protocol_direction_source(
    suite_translation_protocol_direction_t direction)
{
    return direction == SUITE_TR_PROTOCOL_DIRECTION_ZH_EN ? "zh-CN" : "en";
}

const char *suite_translation_protocol_direction_target(
    suite_translation_protocol_direction_t direction)
{
    return direction == SUITE_TR_PROTOCOL_DIRECTION_ZH_EN ? "en" : "zh-CN";
}
