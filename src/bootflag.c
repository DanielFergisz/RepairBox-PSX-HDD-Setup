#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bootflag.h"

#define BOOTMODE_OFFSET 0x14
#define REPARTITION_OFFSET 0x24
#define CONTENTS_OFFSET 0x33

static const char bootmode_value[] = "BOOTMODE=Normal";
static const char repartition_value[] = "repartition=40";
static const char contents_value[] = "contents=1";

const u8 bootflag_expected_sha1[BOOTFLAG_DIGEST_SIZE] = {
    0x7E, 0xF2, 0x24, 0xEE, 0x32, 0x26, 0x1C, 0x62, 0x05, 0x0F,
    0xDC, 0xE5, 0x6D, 0x6D, 0x20, 0x87, 0x21, 0x25, 0x03, 0xAA,
};

_Static_assert(sizeof(bootmode_value) == 16,
               "BOOTMODE payload must include a 16-byte NUL string");
_Static_assert(sizeof(repartition_value) == 15,
               "repartition payload must include a 15-byte NUL string");
_Static_assert(sizeof(contents_value) == 11,
               "contents payload must include an 11-byte NUL string");
_Static_assert(CONTENTS_OFFSET + sizeof(contents_value) == 0x3E,
               "payload strings must end at offset 0x3E");

static u32 rotate_left(u32 value, unsigned int count)
{
    return (value << count) | (value >> (32 - count));
}

static u32 load_be32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | (u32)data[3];
}

static void store_be32(u8 *data, u32 value)
{
    data[0] = (u8)(value >> 24);
    data[1] = (u8)(value >> 16);
    data[2] = (u8)(value >> 8);
    data[3] = (u8)value;
}

static void sha1_transform(u32 state[5], const u8 block[64])
{
    u32 words[80];
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    unsigned int index;

    for (index = 0; index < 16; ++index)
        words[index] = load_be32(block + index * 4);
    for (index = 16; index < 80; ++index) {
        words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^
                                   words[index - 14] ^ words[index - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (index = 0; index < 80; ++index) {
        u32 function;
        u32 constant;
        u32 temporary;

        if (index < 20) {
            function = (b & c) | ((~b) & d);
            constant = 0x5A827999;
        } else if (index < 40) {
            function = b ^ c ^ d;
            constant = 0x6ED9EBA1;
        } else if (index < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8F1BBCDC;
        } else {
            function = b ^ c ^ d;
            constant = 0xCA62C1D6;
        }

        temporary = rotate_left(a, 5) + function + e + constant + words[index];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temporary;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void bootflag_sha1(const void *data, size_t length,
                   u8 digest[BOOTFLAG_DIGEST_SIZE])
{
    const u8 *input = (const u8 *)data;
    u32 state[5] = {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0,
    };
    u8 tail[128];
    size_t full_blocks = length / 64;
    size_t remaining = length % 64;
    size_t padded_size = remaining < 56 ? 64 : 128;
    u64 bit_length = (u64)length * 8;
    size_t index;

    for (index = 0; index < full_blocks; ++index)
        sha1_transform(state, input + index * 64);

    memset(tail, 0, sizeof(tail));
    memcpy(tail, input + full_blocks * 64, remaining);
    tail[remaining] = 0x80;
    for (index = 0; index < 8; ++index) {
        tail[padded_size - 1 - index] = (u8)(bit_length >> (index * 8));
    }

    sha1_transform(state, tail);
    if (padded_size == 128)
        sha1_transform(state, tail + 64);

    for (index = 0; index < 5; ++index)
        store_be32(digest + index * 4, state[index]);
}

int bootflag_verify_image(const u8 bytes[BOOTFLAG_SIZE],
                          u8 calculated_digest[BOOTFLAG_DIGEST_SIZE])
{
    bootflag_sha1(bytes + BOOTFLAG_PAYLOAD_OFFSET,
                  BOOTFLAG_PAYLOAD_SIZE, calculated_digest);
    if (memcmp(bytes, calculated_digest, BOOTFLAG_DIGEST_SIZE) != 0)
        return -EIO;
    if (memcmp(calculated_digest, bootflag_expected_sha1,
               BOOTFLAG_DIGEST_SIZE) != 0)
        return -EINVAL;
    return 0;
}

int bootflag_generate_standard_40gb(bootflag_image_t *image)
{
    if (image == NULL)
        return -EINVAL;

    memset(image, 0, sizeof(*image));
    memcpy(image->bytes + BOOTMODE_OFFSET,
           bootmode_value, sizeof(bootmode_value));
    memcpy(image->bytes + REPARTITION_OFFSET,
           repartition_value, sizeof(repartition_value));
    memcpy(image->bytes + CONTENTS_OFFSET,
           contents_value, sizeof(contents_value));

    bootflag_sha1(image->bytes + BOOTFLAG_PAYLOAD_OFFSET,
                  BOOTFLAG_PAYLOAD_SIZE, image->digest);
    memcpy(image->bytes, image->digest, BOOTFLAG_DIGEST_SIZE);
    image->validation_result =
        bootflag_verify_image(image->bytes, image->digest);
    return image->validation_result;
}

static int key_equals(const u8 *key, size_t length, const char *value)
{
    size_t value_length = strlen(value);

    return length == value_length && memcmp(key, value, length) == 0;
}

static int key_has_prefix(const u8 *key, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);

    return length >= prefix_length &&
           memcmp(key, prefix, prefix_length) == 0;
}

void bootflag_inspect_image(const u8 *bytes, size_t size,
                            bootflag_inspection_t *inspection)
{
    size_t offset;
    int bootmode_count = 0;
    int repartition_count = 0;
    int contents_count = 0;

    memset(inspection, 0, sizeof(*inspection));
    inspection->open_result = -1;
    inspection->final_read_result = -1;
    inspection->extra_read_result = -1;
    inspection->close_result = -1;
    inspection->size = (u32)size;
    inspection->state = BOOTFLAG_STATE_INVALID;

    if (bytes == NULL || size != BOOTFLAG_SIZE)
        return;

    memcpy(inspection->payload, bytes + BOOTFLAG_PAYLOAD_OFFSET,
           BOOTFLAG_PAYLOAD_SIZE);
    bootflag_sha1(inspection->payload, BOOTFLAG_PAYLOAD_SIZE,
                  inspection->calculated_digest);
    inspection->sha1_valid =
        memcmp(bytes, inspection->calculated_digest,
               BOOTFLAG_DIGEST_SIZE) == 0;

    offset = 0;
    while (offset < BOOTFLAG_PAYLOAD_SIZE) {
        size_t end;
        size_t length;
        const u8 *key;

        if (inspection->payload[offset] == '\0') {
            for (end = offset; end < BOOTFLAG_PAYLOAD_SIZE; ++end) {
                if (inspection->payload[end] != '\0')
                    return;
            }
            inspection->payload_valid = 1;
            break;
        }

        for (end = offset; end < BOOTFLAG_PAYLOAD_SIZE; ++end) {
            if (inspection->payload[end] == '\0')
                break;
        }
        if (end == BOOTFLAG_PAYLOAD_SIZE)
            return;

        key = inspection->payload + offset;
        length = end - offset;
        if (key_equals(key, length, "BOOTMODE=Normal")) {
            ++bootmode_count;
            inspection->bootmode_normal = 1;
            if (bootmode_count > 1)
                inspection->conflicting_value = 1;
        } else if (key_has_prefix(key, length, "BOOTMODE=")) {
            inspection->conflicting_value = 1;
        } else if (key_equals(key, length, "repartition=40")) {
            ++repartition_count;
            inspection->repartition_40 = 1;
            if (repartition_count > 1)
                inspection->conflicting_value = 1;
        } else if (key_has_prefix(key, length, "repartition=")) {
            inspection->conflicting_value = 1;
        } else if (key_equals(key, length, "contents=1")) {
            ++contents_count;
            inspection->contents_1 = 1;
            if (contents_count > 1)
                inspection->conflicting_value = 1;
        } else if (key_has_prefix(key, length, "contents=")) {
            inspection->conflicting_value = 1;
        } else {
            u32 unknown = inspection->unknown_key_count;

            if (unknown < BOOTFLAG_MAX_PAYLOAD_KEYS) {
                inspection->unknown_key_offsets[unknown] = (u16)offset;
                inspection->unknown_key_lengths[unknown] = (u16)length;
            }
            ++inspection->unknown_key_count;
        }
        offset = end + 1;
    }

    if (!inspection->payload_valid || !inspection->sha1_valid ||
        inspection->conflicting_value || bootmode_count != 1)
        return;

    if (repartition_count == 0 && contents_count == 0) {
        inspection->state = BOOTFLAG_STATE_NORMAL;
    } else if (repartition_count == 1 && contents_count == 1) {
        inspection->state = BOOTFLAG_STATE_PENDING_40GB;
    }
}

const char *bootflag_state_name(bootflag_state_t state)
{
    switch (state) {
        case BOOTFLAG_STATE_NORMAL:
            return "NORMAL";
        case BOOTFLAG_STATE_PENDING_40GB:
            return "PENDING_40GB";
        default:
            return "INVALID";
    }
}

void bootflag_digest_to_hex(const u8 digest[BOOTFLAG_DIGEST_SIZE],
                            char output[BOOTFLAG_DIGEST_SIZE * 2 + 1])
{
    static const char hex_digits[] = "0123456789ABCDEF";
    unsigned int index;

    for (index = 0; index < BOOTFLAG_DIGEST_SIZE; ++index) {
        output[index * 2] = hex_digits[digest[index] >> 4];
        output[index * 2 + 1] = hex_digits[digest[index] & 0x0F];
    }
    output[BOOTFLAG_DIGEST_SIZE * 2] = '\0';
}
