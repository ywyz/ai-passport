#include "passport_pack.h"

#include <string.h>

static int64_t number_or(const struct passport_json_value *v, int64_t def)
{
    return v && v->kind == PASSPORT_JSON_NUMBER ? v->number : def;
}

int passport_pack_digest_hex_to_bin(const char *hex, size_t hex_len,
                                    uint8_t out[32])
{
    if (hex_len != 64)
        return -1;
    for (size_t i = 0; i < 32; ++i) {
        unsigned int value = 0;
        for (int k = 0; k < 2; ++k) {
            char ch = hex[2 * i + k];
            unsigned int digit;
            if (ch >= '0' && ch <= '9') digit = (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') digit = (unsigned)(ch - 'A' + 10);
            else return -1;
            value = value * 16 + digit;
        }
        out[i] = (uint8_t)value;
    }
    return 0;
}

int passport_pack_check_path(const char *path, size_t path_len)
{
    if (path_len == 0 || path_len >= PASSPORT_PATH_MAX)
        return PASSPORT_PACK_BAD_PATH;
    if (path[0] == '/' || path[0] == '\\' || path[0] == '~')
        return PASSPORT_PACK_BAD_PATH;
    for (size_t i = 0; i < path_len; ++i) {
        if (path[i] == '\\')
            return PASSPORT_PACK_BAD_PATH;
        if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] == 0x7F)
            return PASSPORT_PACK_BAD_PATH;
    }
    if (strncmp(path, "data/", 5) != 0 && strncmp(path, "text/", 5) != 0 &&
            strncmp(path, "fonts/", 6) != 0 && strncmp(path, "audio/", 6) != 0)
        return PASSPORT_PACK_BAD_PATH;
    size_t seg_start = 0;
    for (size_t i = 0; i <= path_len; ++i) {
        if (i == path_len || path[i] == '/') {
            size_t seg_len = i - seg_start;
            if (seg_len == 0)
                return PASSPORT_PACK_BAD_PATH;
            if (seg_len >= 1 && path[seg_start] == '.')
                return PASSPORT_PACK_BAD_PATH; /* '.'/'..'/hidden segments */
            bool ok = true;
            for (size_t k = 0; k < seg_len; ++k) {
                char ch = path[seg_start + k];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                      ch == '_' || ch == ' ')) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                return PASSPORT_PACK_BAD_PATH;
            seg_start = i + 1;
        }
    }
    return PASSPORT_PACK_OK;
}

int passport_pack_preflight(size_t requested_bytes, size_t reserve_bytes,
                            size_t available_bytes)
{
    if (requested_bytes > available_bytes ||
            requested_bytes + reserve_bytes > available_bytes)
        return PASSPORT_PACK_NO_SPACE;
    return PASSPORT_PACK_OK;
}

static int glyph_flag_for(uint32_t cp, struct passport_glyph_set font)
{
    size_t lo = 0, hi = font.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (font.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < font.count && font.codepoints[lo] == cp;
}

int passport_glyph_check_utf8(struct passport_glyph_set font,
                              const uint8_t *text, size_t len,
                              uint32_t *missing_first)
{
    size_t i = 0;
    while (i < len) {
        uint32_t lead = text[i];
        size_t width;
        uint8_t lead_mask;
        if (lead < 0x80) {
            width = 1;
            lead_mask = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            width = 2; lead_mask = 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3; lead_mask = 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4; lead_mask = 0x07;
        } else {
            if (missing_first) *missing_first = 0xFFFFFFFE;
            return PASSPORT_PACK_BAD_SCHEMA;
        }
        if (i + width > len) {
            if (missing_first) *missing_first = 0xFFFFFFFE;
            return PASSPORT_PACK_BAD_SCHEMA;
        }
        uint32_t cp = (uint32_t)(lead & lead_mask);
        bool cont_ok = true;
        for (size_t k = 1; k < width; ++k) {
            uint8_t cont = text[i + k];
            if ((cont & 0xC0) != 0x80) {
                cont_ok = false;
                break;
            }
            cp = (cp << 6) | (uint32_t)(cont & 0x3F);
        }
        if (!cont_ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
                (cp & 0x1FF800) == 0) {
            if (missing_first) *missing_first = cp;
            return PASSPORT_PACK_BAD_SCHEMA;
        }
        if (cp > 0x7F && !glyph_flag_for(cp, font)) {
            if (missing_first) *missing_first = cp;
            return PASSPORT_PACK_GLYPH_MISSING;
        }
        i += width;
    }
    return PASSPORT_PACK_OK;
}

static size_t view_len(const struct passport_json_value *v)
{
    if (!v || v->kind != PASSPORT_JSON_STRING)
        return 0;
    size_t len = 0;
    while (len < v->len && v->raw[len])
        ++len;
    return len;
}

static bool id_chars_ok(const char *s, size_t len, bool allow_slash)
{
    for (size_t i = 0; i < len; ++i) {
        char ch = s[i];
        if (ch == '/') {
            if (allow_slash)
                continue;
            return false;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
                ch == '_' || ch == ' ')
            continue;
        return false;
    }
    return true;
}

static int copy_view(char *dst, size_t cap, const struct passport_json_value *v)
{
    if (!v || v->kind != PASSPORT_JSON_STRING)
        return -1;
    size_t len = view_len(v);
    /* NUL termination via NUL byte sentinel; enabled arbitrarily many bytes */
    if (len == 0 || len >= cap || !id_chars_ok(v->raw, len, false))
        return -1;
    memcpy(dst, v->raw, len);
    dst[len] = '\0';
    return 0;
}

int passport_pack_parse_manifest(const char *json, size_t len,
                                 struct passport_pack_manifest *out)
{
    memset(out, 0, sizeof(*out));
    if (len > PASSPORT_DOC_CAP_BYTES)
        return PASSPORT_PACK_SIZE;
    static struct passport_json_value entries[PASSPORT_JSON_MAX_ENTRIES];
    static char scratch[8192];
    struct passport_json_doc doc = {.entries = entries,
                                    .entries_cap = PASSPORT_JSON_MAX_ENTRIES,
                                    .scratch = scratch,
                                    .scratch_cap = sizeof(scratch)};
    if (passport_json_parse(json, len, &doc) != 0)
        return PASSPORT_PACK_BAD_SCHEMA;
    const struct passport_json_value *root = &doc.entries[0];
    if (root->kind != PASSPORT_JSON_OBJECT)
        return PASSPORT_PACK_BAD_SCHEMA;
    if (number_or(passport_json_member(&doc, root, "schema_version"), 0) != 1)
        return PASSPORT_PACK_BAD_SCHEMA;
    const struct passport_json_value *pack_id = passport_json_member(&doc, root, "pack_id");
    if (copy_view(out->pack_id, sizeof(out->pack_id), pack_id) < 0)
        return PASSPORT_PACK_BAD_SCHEMA;
    const struct passport_json_value *revision = passport_json_member(&doc, root, "revision");
    if (!revision || revision->kind != PASSPORT_JSON_NUMBER ||
            revision->number <= 0 || revision->number > 0x7FFFFFFF)
        return PASSPORT_PACK_BAD_SCHEMA;
    out->revision = (uint32_t)revision->number;
    const struct passport_json_value *fixture = passport_json_member(&doc, root, "fixture");
    out->fixture = fixture && fixture->kind == PASSPORT_JSON_TRUE;
    const struct passport_json_value *total_v = passport_json_member(&doc, root, "total_bytes");
    if (!total_v || total_v->kind != PASSPORT_JSON_NUMBER ||
            total_v->number < 0 || total_v->number > (int64_t)PASSPORT_DOC_CAP_BYTES)
        return PASSPORT_PACK_BAD_SCHEMA;
    out->total_bytes = (size_t)total_v->number;
    const struct passport_json_value *kind = passport_json_member(&doc, root, "kind");
    if (kind && kind->kind == PASSPORT_JSON_STRING) {
        size_t klen = view_len(kind);
        if (klen >= sizeof(out->kind))
            return PASSPORT_PACK_BAD_SCHEMA;
        memcpy(out->kind, kind->raw, klen);
        out->kind[klen] = '\0';
    }
    if (out->fixture && out->total_bytes > PASSPORT_FIXTURE_CAP_BYTES)
        return PASSPORT_PACK_SIZE;

    const struct passport_json_value *glyphs = passport_json_member(&doc, root, "glyph_inventory");
    if (glyphs && glyphs->kind == PASSPORT_JSON_ARRAY) {
        size_t pos = glyphs->start;
        size_t end = glyphs->start + passport_json_subtree_size(&doc, (size_t)(glyphs - doc.entries)) - 1;
        while (pos < end && out->glyph_count < 256) {
            const struct passport_json_value *v =
                passport_json_array_next(&doc, &pos, end);
            if (!v || v->kind != PASSPORT_JSON_NUMBER || v->number < 0 ||
                    v->number > 0x10FFFF)
                return PASSPORT_PACK_BAD_SCHEMA;
            out->glyph_inventory[out->glyph_count++] = (uint32_t)v->number;
        }
        for (size_t i = 1; i < out->glyph_count; ++i) {
            if (out->glyph_inventory[i - 1] > out->glyph_inventory[i])
                return PASSPORT_PACK_BAD_SCHEMA;
        }
    }

    const struct passport_json_value *targets = passport_json_member(&doc, root, "route_stops");
    if (targets && targets->kind == PASSPORT_JSON_OBJECT) {
        const struct passport_json_value *route = passport_json_member(&doc, targets, "route_id");
        if (!route || copy_view(out->route_id, sizeof(out->route_id), route) < 0)
            return PASSPORT_PACK_BAD_SCHEMA;
        const struct passport_json_value *stops = passport_json_member(&doc, targets, "stop_ids");
        if (!stops || stops->kind != PASSPORT_JSON_ARRAY)
            return PASSPORT_PACK_BAD_SCHEMA;
        size_t pos = stops->start;
        size_t stop_index = (size_t)(stops - doc.entries);
        size_t end = stops->start + passport_json_subtree_size(&doc, stop_index) - 1;
        while (pos < end && out->route_stop_count < 16) {
            const struct passport_json_value *v =
                passport_json_array_next(&doc, &pos, end);
            if (!v || copy_view(out->route_stop_ids[out->route_stop_count],
                                PASSPORT_ID_MAX + 1, v) < 0)
                return PASSPORT_PACK_BAD_SCHEMA;
            ++out->route_stop_count;
        }
        if (out->route_stop_count == 0)
            return PASSPORT_PACK_BAD_SCHEMA;
    }

    const struct passport_json_value *files = passport_json_member(&doc, root, "files");
    if (!files || files->kind != PASSPORT_JSON_ARRAY)
        return PASSPORT_PACK_BAD_SCHEMA;
    if (files->len > PASSPORT_PACK_MAX_FILES)
        return PASSPORT_PACK_SIZE;
    size_t pos = files->start;
    size_t files_index = (size_t)(files - doc.entries);
    size_t end = files->start + passport_json_subtree_size(&doc, files_index) - 1;
    while (pos < end) {
        const struct passport_json_value *obj = passport_json_array_next(&doc, &pos, end);
        if (!obj)
            return PASSPORT_PACK_BAD_SCHEMA;
        if (obj->kind != PASSPORT_JSON_OBJECT)
            return PASSPORT_PACK_BAD_SCHEMA;
        const struct passport_json_value *path = passport_json_member(&doc, obj, "path");
        const struct passport_json_value *file_id = passport_json_member(&doc, obj, "file_id");
        const struct passport_json_value *sha = passport_json_member(&doc, obj, "sha256");
        const struct passport_json_value *size = passport_json_member(&doc, obj, "size_bytes");
        const struct passport_json_value *media = passport_json_member(&doc, obj, "media_type");
        if (!path || !file_id || !sha || !size || !media)
            return PASSPORT_PACK_BAD_SCHEMA;
        struct passport_pack_file *file = &out->files[out->file_count];
        memset(file, 0, sizeof(*file));
        if (path->len == 0 || path->len >= PASSPORT_PATH_MAX ||
                passport_pack_check_path(path->raw, path->len) != 0)
            return PASSPORT_PACK_BAD_PATH;
        memcpy(file->path, path->raw, path->len);
        file->path[path->len] = '\0';
        if (copy_view(file->file_id, sizeof(file->file_id), file_id) < 0)
            return PASSPORT_PACK_BAD_SCHEMA;
        if (sha->kind != PASSPORT_JSON_STRING || sha->len != 64 ||
                passport_pack_digest_hex_to_bin(sha->raw, sha->len, file->sha256) != 0)
            return PASSPORT_PACK_BAD_SCHEMA;
        /* digest comparison happens at activation (per-file content); the
         * manifest parse validates only shape here */
        if (size->kind != PASSPORT_JSON_NUMBER || size->number <= 0 ||
                size->number > (int64_t)PASSPORT_DOC_CAP_BYTES)
            return PASSPORT_PACK_SIZE;
        file->size_bytes = (size_t)size->number;
        if (media->kind != PASSPORT_JSON_STRING || media->len == 0 ||
                media->len >= sizeof(file->media_type))
            return PASSPORT_PACK_BAD_SCHEMA;
        memcpy(file->media_type, media->raw, media->len);
        file->media_type[media->len] = '\0';
        out->file_count++;
    }
    if (out->file_count == 0 || out->file_count > PASSPORT_PACK_MAX_FILES)
        return PASSPORT_PACK_SIZE;
    /* sum must equal total (A03 boundary rule) */
    size_t sum = 0;
    for (size_t i = 0; i < out->file_count; ++i)
        sum += out->files[i].size_bytes;
    if (sum != out->total_bytes)
        return PASSPORT_PACK_BAD_SCHEMA;
    if (out->fixture && sum > PASSPORT_FIXTURE_CAP_BYTES)
        return PASSPORT_PACK_SIZE;
    return PASSPORT_PACK_OK;
}
