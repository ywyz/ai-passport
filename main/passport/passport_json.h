/**
 * passport_json.h - bounded, flat-table JSON scanner for manifests (D1b).
 *
 * Strings are views into the caller's input buffer; escape sequences are
 * decoded into a small scratch keyed by comparisons. Containers record the
 * first child entry and direct-child count; iteration uses bounded subtree
 * sizes. Depth/count limits fail closed for malformed input (A03).
 */
#ifndef PASSPORT_JSON_H
#define PASSPORT_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSPORT_JSON_MAX_DEPTH 12
#define PASSPORT_JSON_MAX_ENTRIES 4096

enum passport_json_kind {
    PASSPORT_JSON_NULL = 0,
    PASSPORT_JSON_TRUE,
    PASSPORT_JSON_FALSE,
    PASSPORT_JSON_NUMBER,
    PASSPORT_JSON_STRING,
    PASSPORT_JSON_ARRAY,
    PASSPORT_JSON_OBJECT,
};

struct passport_json_value {
    uint8_t kind;
    uint8_t pad;
    uint32_t start;   /* containers: first child entry index */
    uint32_t len;     /* containers: pair/element count; strings: bytes */
    int64_t number;
    const char *raw;
};

struct passport_json_doc {
    struct passport_json_value *entries;
    size_t entries_cap;
    size_t entries_len;
    char *scratch;
    size_t scratch_cap;
    size_t scratch_len;
};

int passport_json_parse(const char *text, size_t len, struct passport_json_doc *doc);

/* direct member lookup by name */
const struct passport_json_value *passport_json_member(
    const struct passport_json_doc *doc, const struct passport_json_value *obj,
    const char *name);

/* array element iteration: *pos steps past element subtrees */
const struct passport_json_value *passport_json_array_next(
    const struct passport_json_doc *doc, size_t *pos, size_t end);

size_t passport_json_subtree_size(const struct passport_json_doc *doc,
                                  size_t index);

bool passport_json_streq(const struct passport_json_value *v, const char *s);

#endif /* PASSPORT_JSON_H */
