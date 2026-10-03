#include "passport_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct passport_sha256 *self, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = self->h[0], b = self->h[1], c = self->h[2], d = self->h[3];
    uint32_t e = self->h[4], f = self->h[5], g = self->h[6], h = self->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    self->h[0] += a; self->h[1] += b; self->h[2] += c; self->h[3] += d;
    self->h[4] += e; self->h[5] += f; self->h[6] += g; self->h[7] += h;
}

void passport_sha256_init(struct passport_sha256 *self)
{
    static const uint32_t H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(self->h, H0, sizeof(H0));
    self->bits = 0;
    self->keep = 0;
}

void passport_sha256_update(struct passport_sha256 *self,
                            const uint8_t *data, size_t len)
{
    self->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t fill = 64 - self->keep;
        size_t take = len < fill ? len : fill;
        memcpy(self->block + self->keep, data, take);
        self->keep += take;
        data += take;
        len -= take;
        if (self->keep == 64) {
            sha256_block(self, self->block);
            self->keep = 0;
        }
    }
}

void passport_sha256_final(struct passport_sha256 *self, uint8_t out[32])
{
    uint64_t bits = self->bits;
    uint8_t pad = 0x80;
    passport_sha256_update(self, &pad, 1);
    uint8_t zero = 0;
    while (self->keep != 56) {
        passport_sha256_update(self, &zero, 1);
    }
    uint8_t length_bits[8];
    for (int i = 7; i >= 0; --i) {
        length_bits[i] = (uint8_t)(bits & 0xFF);
        bits >>= 8;
    }
    /* append length manually (block now exactly 56 keep-free) */
    memcpy(self->block + 56, length_bits, 8);
    sha256_block(self, self->block);
    self->keep = 0;
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = (uint8_t)(self->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(self->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(self->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)self->h[i];
    }
}
