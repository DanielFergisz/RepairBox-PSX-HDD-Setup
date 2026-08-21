#ifndef REPAIRBOX_SHA256_H
#define REPAIRBOX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct sha256_context {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_used;
} sha256_context_t;

void sha256_init(sha256_context_t *context);
void sha256_update(sha256_context_t *context, const void *data, size_t size);
void sha256_final(sha256_context_t *context,
                  uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                   char output[SHA256_DIGEST_SIZE * 2 + 1]);

#endif
