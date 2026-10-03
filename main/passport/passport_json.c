#include "passport_json.h"

#include <string.h>

#include "passport_json_decodeutf8.h"

static const char *skip_ws(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\n' ||
                            *cursor == '\r' || *cursor == '\t'))
        ++cursor;
    return cursor;
}

static struct passport_json_value *doc_entry(struct passport_json_doc *doc)
{
    if (doc->entries_len >= doc->entries_cap)
        return NULL;
    struct passport_json_value *entry = &doc->entries[doc->entries_len++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static bool scratch_room(const struct passport_json_doc *doc, size_t add)
{
    return doc->scratch_len + add <= doc->scratch_cap;
}

static int parse_string(const char **cursor, const char *end,
                        struct passport_json_doc *doc,
                        struct passport_json_value *out)
{
    if (*cursor >= end || **cursor != '"')
        return -1;
    ++*cursor;
    const char *begin = *cursor;
    bool escaped = false;
    while (*cursor < end && **cursor != '"') {
        if (**cursor == '\\') {
            escaped = true;
            ++*cursor;
            if (*cursor >= end)
                return -1;
        }
        ++*cursor;
    }
    if (*cursor >= end)
        return -1;
    const char *stop = *cursor;
    ++*cursor;
    out->kind = PASSPORT_JSON_STRING;
    if ((size_t)(stop - begin) > 0xFFFF)
        return -1;
    out->len = (uint16_t)(stop - begin);
    out->raw = begin;
    if (!escaped)
        return 0;
    long base = (long)doc->scratch_len;
    for (const char *scan = begin; scan < stop; ++scan) {
        if (*scan != '\\') {
            if (!scratch_room(doc, 1))
                return -1;
            doc->scratch[doc->scratch_len++] = *scan;
            continue;
        }
        ++scan;
        if (scan >= stop)
            return -1;
        char esc = *scan;
        switch (esc) {
        case '"': case '\\': case '/':
            if (!scratch_room(doc, 1)) return -1;
            doc->scratch[doc->scratch_len++] = esc;
            break;
        case 'n': case 'r': case 't': case 'b': case 'f':
            if (!scratch_room(doc, 1)) return -1;
            doc->scratch[doc->scratch_len++] =
                esc == 'n' ? '\n' : esc == 'r' ? '\r' :
                esc == 't' ? '\t' : esc == 'b' ? '\b' : '\f';
            break;
        case 'u': {
            if (stop - scan < 5)
                return -1;
            unsigned int cp = 0;
            for (int k = 1; k <= 4; ++k) {
                char hex = scan[k];
                unsigned int digit;
                if (hex >= '0' && hex <= '9') digit = (unsigned)(hex - '0');
                else if (hex >= 'a' && hex <= 'f') digit = (unsigned)(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') digit = (unsigned)(hex - 'A' + 10);
                else return -1;
                cp = cp * 16 + digit;
            }
            scan += 4;
            int encoded = passport_json_utf8_encode(
                cp, doc->scratch + doc->scratch_len,
                doc->scratch_cap - doc->scratch_len);
            if (encoded <= 0) return -1;
            doc->scratch_len += (size_t)encoded;
            break;
        }
        default:
            return -1;
        }
    }
    if (!scratch_room(doc, 1))
        return -1;
    doc->scratch[doc->scratch_len++] = '\0';
    out->raw = doc->scratch + base;
    return 0;
}

static int parse_number(const char **cursor, const char *end,
                        struct passport_json_value *out)
{
    bool negative = false;
    if (*cursor < end && **cursor == '-') {
        negative = true;
        ++*cursor;
    }
    int64_t value = 0;
    bool any = false;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        any = true;
        ++*cursor;
    }
    if (!any)
        return -1;
    if (*cursor < end && (**cursor == '.' || **cursor == 'e' || **cursor == 'E'))
        return -1;
    out->kind = PASSPORT_JSON_NUMBER;
    out->number = negative ? -value : value;
    return 0;
}

static int parse_value(struct passport_json_doc *doc, const char **cursor,
                       const char *end, int depth);

/* container: creates entry at index -1 relative offset handled by caller */
static int parse_container(struct passport_json_doc *doc, const char **cursor,
                           const char *end, int depth, bool object,
                           struct passport_json_value *entry)
{
    ++*cursor; /* opener consumed by caller */
    if (depth > PASSPORT_JSON_MAX_DEPTH)
        return -1;
    *cursor = skip_ws(*cursor, end);
    size_t count = 0;
    entry->kind = object ? PASSPORT_JSON_OBJECT : PASSPORT_JSON_ARRAY;
    entry->start = doc->entries_len; /* first child entry (key pairs/value) */
    if (*cursor < end && **cursor == (object ? '}' : ']')) {
        ++*cursor;
        entry->len = 0;
        return 0;
    }
    for (;;) {
        if (object) {
            struct passport_json_value *key = doc_entry(doc);
            if (!key)
                return -1;
            if (parse_string(cursor, end, doc, key) != 0)
                return -1;
            *cursor = skip_ws(*cursor, end);
            if (*cursor >= end || **cursor != ':')
                return -1;
            ++*cursor;
        }
        if (parse_value(doc, cursor, end, depth + 1) != 0)
            return -1;
        ++count;
        *cursor = skip_ws(*cursor, end);
        if (*cursor < end && **cursor == ',') {
            ++*cursor;
            continue;
        }
        if (*cursor < end && **cursor == (object ? '}' : ']')) {
            ++*cursor;
            break;
        }
        return -1;
    }
    entry->len = count;
    return 0;
}

static int parse_value(struct passport_json_doc *doc, const char **cursor,
                       const char *end, int depth)
{
    if (depth > PASSPORT_JSON_MAX_DEPTH)
        return -1;
    *cursor = skip_ws(*cursor, end);
    if (*cursor >= end)
        return -1;
    struct passport_json_value *entry = doc_entry(doc);
    if (!entry)
        return -1;
    if (**cursor == '"')
        return parse_string(cursor, end, doc, entry);
    if (**cursor == '-' || (**cursor >= '0' && **cursor <= '9'))
        return parse_number(cursor, end, entry);
    if (**cursor == '{')
        return parse_container(doc, cursor, end, depth, true, entry);
    if (**cursor == '[')
        return parse_container(doc, cursor, end, depth, false, entry);
    if (end - *cursor >= 4 && strncmp(*cursor, "true", 4) == 0) {
        entry->kind = PASSPORT_JSON_TRUE;
        *cursor += 4;
        return 0;
    }
    if (end - *cursor >= 5 && strncmp(*cursor, "false", 5) == 0) {
        entry->kind = PASSPORT_JSON_FALSE;
        *cursor += 5;
        return 0;
    }
    if (end - *cursor >= 4 && strncmp(*cursor, "null", 4) == 0) {
        entry->kind = PASSPORT_JSON_NULL;
        *cursor += 4;
        return 0;
    }
    return -1;
}

int passport_json_parse(const char *text, size_t len, struct passport_json_doc *doc)
{
    const char *cursor = text;
    const char *end = text + len;
    doc->entries_len = 0;
    doc->scratch_len = 0;
    if (parse_value(doc, &cursor, end, 0) != 0)
        return -1;
    cursor = skip_ws(cursor, end);
    return cursor == end ? 0 : -1;
}

size_t passport_json_subtree_size(const struct passport_json_doc *doc,
                                  size_t index)
{
    if (index >= doc->entries_len)
        return 1;
    const struct passport_json_value *entry = &doc->entries[index];
    if (entry->kind != PASSPORT_JSON_ARRAY &&
        entry->kind != PASSPORT_JSON_OBJECT)
        return 1;
    size_t total = 1;
    size_t pos = entry->start;
    for (size_t m = 0; m < entry->len && pos < doc->entries_len; ++m) {
        if (entry->kind == PASSPORT_JSON_OBJECT) {
            size_t value_size = passport_json_subtree_size(doc, pos + 1);
            pos += 1 + value_size;
            total += 1 + value_size;
            if (pos > doc->entries_len) return total;
        } else {
            size_t element_size = passport_json_subtree_size(doc, pos);
            pos += element_size;
            total += element_size;
            if (pos > doc->entries_len) return total;
        }
    }
    return total;
}

const struct passport_json_value *passport_json_array_next(
    const struct passport_json_doc *doc, size_t *pos, size_t end)
{
    if (*pos >= end || *pos >= doc->entries_len)
        return NULL;
    const struct passport_json_value *value = &doc->entries[*pos];
    *pos += passport_json_subtree_size(doc, *pos);
    return value;
}

bool passport_json_streq(const struct passport_json_value *v, const char *s)
{
    if (!v || v->kind != PASSPORT_JSON_STRING)
        return false;
    size_t len = strlen(s);
    return v->len == len && memcmp(v->raw, s, len) == 0;
}

const struct passport_json_value *passport_json_member(
    const struct passport_json_doc *doc, const struct passport_json_value *obj,
    const char *name)
{
    if (obj->kind != PASSPORT_JSON_OBJECT)
        return NULL;
    size_t pos = obj->start;
    for (size_t i = 0; i < obj->len; ++i) {
        if (pos + 1 >= doc->entries_len)
            return NULL;
        if (passport_json_streq(&doc->entries[pos], name))
            return &doc->entries[pos + 1];
        pos += 1 + passport_json_subtree_size(doc, pos + 1);
    }
    return NULL;
}
