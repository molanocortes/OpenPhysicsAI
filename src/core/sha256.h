/* sha256.h - SHA-256 (FIPS 180-4) for geometry, specification and result fingerprints */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Sha256 {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t fill;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, unsigned char digest[32]);
void sha256_hex(const unsigned char digest[32], char out[65]);
void sha256_hex_of(const void *data, size_t n, char out[65]);
/* Hashes a whole file; size (optional) receives its length. */
bool sha256_file(const char *path, char out[65], uint64_t *size);
