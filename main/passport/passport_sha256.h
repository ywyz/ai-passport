/**
 * passport_sha256.h / .c - compact SHA-256 for manifest/record digests.
 * Not side-channel hardened beyond branchless arithmetic: it hashes public
 * content digests, never secrets. Secret digests use more code paths.
 */
#ifndef PASSPORT_SHA256_H
#define PASSPORT_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct passport_sha256 {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t keep; /* bytes buffered in block */
};

void passport_sha256_init(struct passport_sha256 *self);
void passport_sha256_update(struct passport_sha256 *self,
                            const uint8_t *data, size_t len);
void passport_sha256_final(struct passport_sha256 *self, uint8_t out[32]);

#endif /* PASSPORT_SHA256_H */
