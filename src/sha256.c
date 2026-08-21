#include <string.h>

#include "sha256.h"

static const uint32_t round_constants[64] = {
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

static uint32_t rotate_right(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t read_be32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static void transform(sha256_context_t *context, const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int index;

    for (index = 0; index < 16; ++index)
        words[index] = read_be32(block + index * 4u);
    for (; index < 64; ++index) {
        uint32_t s0 = rotate_right(words[index - 15], 7) ^
                      rotate_right(words[index - 15], 18) ^
                      (words[index - 15] >> 3);
        uint32_t s1 = rotate_right(words[index - 2], 17) ^
                      rotate_right(words[index - 2], 19) ^
                      (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (index = 0; index < 64; ++index) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + sum1 + choose +
                              round_constants[index] + words[index];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void sha256_init(sha256_context_t *context)
{
    static const uint32_t initial_state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    memcpy(context->state, initial_state, sizeof(initial_state));
    context->total_bytes = 0;
    context->block_used = 0;
}

void sha256_update(sha256_context_t *context, const void *data, size_t size)
{
    const uint8_t *input = data;

    context->total_bytes += size;
    while (size != 0) {
        size_t available = sizeof(context->block) - context->block_used;
        size_t amount = size < available ? size : available;

        memcpy(context->block + context->block_used, input, amount);
        context->block_used += amount;
        input += amount;
        size -= amount;
        if (context->block_used == sizeof(context->block)) {
            transform(context, context->block);
            context->block_used = 0;
        }
    }
}

void sha256_final(sha256_context_t *context,
                  uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bit_count = context->total_bytes * 8u;
    unsigned int index;

    context->block[context->block_used++] = 0x80u;
    if (context->block_used > 56) {
        memset(context->block + context->block_used, 0,
               sizeof(context->block) - context->block_used);
        transform(context, context->block);
        context->block_used = 0;
    }
    memset(context->block + context->block_used, 0,
           56u - context->block_used);
    for (index = 0; index < 8; ++index)
        context->block[63u - index] = (uint8_t)(bit_count >> (index * 8u));
    transform(context, context->block);

    for (index = 0; index < 8; ++index) {
        digest[index * 4u] = (uint8_t)(context->state[index] >> 24);
        digest[index * 4u + 1u] =
            (uint8_t)(context->state[index] >> 16);
        digest[index * 4u + 2u] =
            (uint8_t)(context->state[index] >> 8);
        digest[index * 4u + 3u] = (uint8_t)context->state[index];
    }
    memset(context, 0, sizeof(*context));
}

void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                   char output[SHA256_DIGEST_SIZE * 2 + 1])
{
    static const char digits[] = "0123456789abcdef";
    unsigned int index;

    for (index = 0; index < SHA256_DIGEST_SIZE; ++index) {
        output[index * 2u] = digits[digest[index] >> 4];
        output[index * 2u + 1u] = digits[digest[index] & 0x0fu];
    }
    output[SHA256_DIGEST_SIZE * 2u] = '\0';
}
