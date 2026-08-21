#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "bootflag_ro.h"

static int allow_pending_reinstall;

void bootflag_ro_allow_pending_reinstall(int allow)
{
    allow_pending_reinstall = allow != 0;
}

static u32 rotate_left(u32 value, unsigned int count)
{
    return (value << count) | (value >> (32u - count));
}

static u32 load_be32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | data[3];
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
    u32 a, b, c, d, e;
    unsigned int index;

    for (index = 0; index < 16; ++index)
        words[index] = load_be32(block + index * 4u);
    for (index = 16; index < 80; ++index)
        words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^
                                   words[index - 14] ^ words[index - 16], 1);
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (index = 0; index < 80; ++index) {
        u32 function;
        u32 constant;
        u32 temporary;

        if (index < 20) {
            function = (b & c) | ((~b) & d);
            constant = 0x5A827999u;
        } else if (index < 40) {
            function = b ^ c ^ d;
            constant = 0x6ED9EBA1u;
        } else if (index < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8F1BBCDCu;
        } else {
            function = b ^ c ^ d;
            constant = 0xCA62C1D6u;
        }
        temporary = rotate_left(a, 5) + function + e + constant + words[index];
        e = d; d = c; c = rotate_left(b, 30); b = a; a = temporary;
    }
    state[0] += a; state[1] += b; state[2] += c;
    state[3] += d; state[4] += e;
}

static void sha1(const void *data, size_t length,
                 u8 digest[BOOTFLAG_RO_DIGEST_SIZE])
{
    const u8 *input = data;
    u32 state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                    0x10325476u, 0xC3D2E1F0u};
    u8 tail[128];
    size_t full_blocks = length / 64u;
    size_t remaining = length % 64u;
    size_t padded_size = remaining < 56u ? 64u : 128u;
    u64 bit_length = (u64)length * 8u;
    size_t index;

    for (index = 0; index < full_blocks; ++index)
        sha1_transform(state, input + index * 64u);
    memset(tail, 0, sizeof(tail));
    memcpy(tail, input + full_blocks * 64u, remaining);
    tail[remaining] = 0x80;
    for (index = 0; index < 8; ++index)
        tail[padded_size - 1u - index] = (u8)(bit_length >> (index * 8u));
    sha1_transform(state, tail);
    if (padded_size == 128u)
        sha1_transform(state, tail + 64);
    for (index = 0; index < 5; ++index)
        store_be32(digest + index * 4u, state[index]);
}

static int key_equals(const u8 *key, size_t length, const char *value)
{
    return length == strlen(value) && memcmp(key, value, length) == 0;
}

static int key_prefix(const u8 *key, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);

    return length >= prefix_length &&
           memcmp(key, prefix, prefix_length) == 0;
}

static void inspect_image(const u8 bytes[BOOTFLAG_RO_SIZE],
                          bootflag_ro_result_t *result)
{
    const u8 *payload = bytes + BOOTFLAG_RO_PAYLOAD_OFFSET;
    size_t offset = 0;
    int bootmode_count = 0;
    int repartition_count = 0;
    int contents_count = 0;

    sha1(payload, BOOTFLAG_RO_PAYLOAD_SIZE, result->calculated_digest);
    result->sha1_valid =
        memcmp(bytes, result->calculated_digest,
               BOOTFLAG_RO_DIGEST_SIZE) == 0;
    while (offset < BOOTFLAG_RO_PAYLOAD_SIZE) {
        size_t end;
        size_t length;
        const u8 *key;

        if (payload[offset] == '\0') {
            for (end = offset; end < BOOTFLAG_RO_PAYLOAD_SIZE; ++end) {
                if (payload[end] != '\0')
                    return;
            }
            result->payload_valid = 1;
            break;
        }
        for (end = offset; end < BOOTFLAG_RO_PAYLOAD_SIZE; ++end) {
            if (payload[end] == '\0')
                break;
        }
        if (end == BOOTFLAG_RO_PAYLOAD_SIZE)
            return;
        key = payload + offset;
        length = end - offset;
        if (key_equals(key, length, "BOOTMODE=Normal")) {
            result->bootmode_normal = 1;
            if (++bootmode_count > 1)
                result->conflicting_value = 1;
        } else if (key_prefix(key, length, "BOOTMODE=")) {
            result->conflicting_value = 1;
        } else if (key_equals(key, length, "repartition=40")) {
            result->repartition_40 = 1;
            if (++repartition_count > 1)
                result->conflicting_value = 1;
        } else if (key_prefix(key, length, "repartition=")) {
            result->conflicting_value = 1;
        } else if (key_equals(key, length, "contents=1")) {
            result->contents_1 = 1;
            if (++contents_count > 1)
                result->conflicting_value = 1;
        } else if (key_prefix(key, length, "contents=")) {
            result->conflicting_value = 1;
        } else {
            ++result->unknown_key_count;
        }
        offset = end + 1u;
    }
    if (!result->payload_valid || !result->sha1_valid ||
        result->conflicting_value || bootmode_count != 1)
        return;
    if (repartition_count == 0 && contents_count == 0)
        result->state = BOOTFLAG_RO_NORMAL;
    else if (repartition_count == 1 && contents_count == 1)
        result->state = BOOTFLAG_RO_PENDING_40GB;
}

void bootflag_ro_inspect(bootflag_ro_result_t *result)
{
    u8 bytes[BOOTFLAG_RO_SIZE];
    u8 extra;
    int fd;
    int offset = 0;

    memset(result, 0, sizeof(*result));
    result->open_result = -1;
    result->final_read_result = -1;
    result->extra_read_result = -1;
    result->close_result = -1;
    result->state = BOOTFLAG_RO_INVALID;
    fd = fileXioOpen(BOOTFLAG_RO_XFROM_PATH, FIO_O_RDONLY, 0);
    result->open_result = fd;
    result->present = fd >= 0;
    if (fd < 0)
        return;
    while (offset < BOOTFLAG_RO_SIZE) {
        result->final_read_result =
            fileXioRead(fd, bytes + offset, BOOTFLAG_RO_SIZE - offset);
        if (result->final_read_result <= 0)
            break;
        offset += result->final_read_result;
    }
    if (offset == BOOTFLAG_RO_SIZE)
        result->extra_read_result = fileXioRead(fd, &extra, 1);
    result->close_result = fileXioClose(fd);
    result->size = (u32)offset +
                   (result->extra_read_result > 0 ? 1u : 0u);
    if (offset == BOOTFLAG_RO_SIZE && result->extra_read_result == 0 &&
        result->close_result >= 0)
        inspect_image(bytes, result);
    if (allow_pending_reinstall &&
        result->state == BOOTFLAG_RO_PENDING_40GB) {
        result->pending_40gb_preserved = 1;
        result->state = BOOTFLAG_RO_NORMAL;
    }
}

const char *bootflag_ro_state_name(bootflag_ro_state_t state)
{
    if (state == BOOTFLAG_RO_NORMAL)
        return "NORMAL";
    if (state == BOOTFLAG_RO_PENDING_40GB)
        return "PENDING_40GB";
    return "INVALID";
}
