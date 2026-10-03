/**
 * passport_pack.h - resource manifest model + activation checks (D1b).
 *
 * Pure logic (A03, DR05): manifest parse, per-file digest compare, path
 * normalization, size preflight, glyph coverage, all against caller-supplied
 * tables so host tests exercise rejection rules without a device.
 */
#ifndef PASSPORT_PACK_H
#define PASSPORT_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "passport_json.h"
#include "passport_sha256.h"

#define PASSPORT_PACK_MAX_FILES 128
#define PASSPORT_PATH_MAX 160
#define PASSPORT_ID_MAX 64
#define PASSPORT_FIXTURE_CAP_BYTES (768 * 1024)
#define PASSPORT_DOC_CAP_BYTES (10 * 1024 * 1024)

enum passport_pack_error {
    PASSPORT_PACK_OK = 0,
    PASSPORT_PACK_BAD_SCHEMA = 1,
    PASSPORT_PACK_BAD_PATH = 2,        /* traversal/absolute/etc. */
    PASSPORT_PACK_SIZE = 3,
    PASSPORT_PACK_DIGIT = 4,           /* digest mismatch */
    PASSPORT_PACK_GLYPH_MISSING = 5,
    PASSPORT_PACK_NO_SPACE = 6,
    PASSPORT_PACK_IO = 7,
    PASSPORT_PACK_NOT_FOUND = 8,
};

struct passport_pack_file {
    char file_id[PASSPORT_ID_MAX + 1];
    char path[PASSPORT_PATH_MAX];
    size_t size_bytes;
    uint8_t sha256[32];
    char media_type[40];
};

struct passport_pack_manifest {
    char pack_id[PASSPORT_ID_MAX + 1];
    uint32_t revision;
    char kind[16];
    bool fixture;
    size_t total_bytes;
    uint32_t glyph_inventory[256];
    size_t glyph_count;
    char route_id[PASSPORT_ID_MAX + 1];       /* guide_targets, optional */
    char route_stop_ids[16][PASSPORT_ID_MAX + 1];
    size_t route_stop_count;
    struct passport_pack_file files[PASSPORT_PACK_MAX_FILES];
    size_t file_count;
};

int passport_pack_parse_manifest(const char *json, size_t len,
                                 struct passport_pack_manifest *out);

/* normalized-path gate (A03) */
int passport_pack_check_path(const char *path, size_t path_len);

/* compare one digest hex (64 chars) against a binary digest */
int passport_pack_digest_hex_to_bin(const char *hex, size_t hex_len, uint8_t out[32]);

/* preflight: requested bytes + declared reserve against available */
int passport_pack_preflight(size_t requested_bytes, size_t reserve_bytes,
                            size_t available_bytes);

/* glyph coverage: scan UTF-8 text for code points not in the device font */
struct passport_glyph_set {
    const uint32_t *codepoints; /* sorted ascending */
    size_t count;
};
int passport_glyph_check_utf8(struct passport_glyph_set font,
                              const uint8_t *text, size_t len,
                              uint32_t *missing_first);

#endif /* PASSPORT_PACK_H */
