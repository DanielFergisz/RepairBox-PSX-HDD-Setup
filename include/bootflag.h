#ifndef REPAIRBOX_BOOTFLAG_H
#define REPAIRBOX_BOOTFLAG_H

#include <stddef.h>

#ifdef _EE
#include <tamtypes.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define BOOTFLAG_SIZE 512
#define BOOTFLAG_DIGEST_SIZE 20
#define BOOTFLAG_PAYLOAD_OFFSET 0x14
#define BOOTFLAG_PAYLOAD_SIZE (BOOTFLAG_SIZE - BOOTFLAG_PAYLOAD_OFFSET)
#define BOOTFLAG_XFROM_PATH "xfrom0:/BIEXEC-SYSTEM/bootflag.txt"
#define BOOTFLAG_EXPECTED_SHA1_HEX "7EF224EE32261C62050FDCE56D6D2087212503AA"
#define BOOTFLAG_MAX_PAYLOAD_KEYS (BOOTFLAG_PAYLOAD_SIZE / 2)

typedef enum bootflag_state {
    BOOTFLAG_STATE_NORMAL = 0,
    BOOTFLAG_STATE_PENDING_40GB,
    BOOTFLAG_STATE_INVALID,
} bootflag_state_t;

typedef struct bootflag_image {
    u8 bytes[BOOTFLAG_SIZE];
    u8 digest[BOOTFLAG_DIGEST_SIZE];
    int validation_result;
} bootflag_image_t;

typedef struct bootflag_inspection {
    int open_result;
    int final_read_result;
    int extra_read_result;
    int close_result;
    u32 size;
    int present;
    int sha1_valid;
    int payload_valid;
    int bootmode_normal;
    int repartition_40;
    int contents_1;
    int conflicting_value;
    bootflag_state_t state;
    u32 unknown_key_count;
    u8 payload[BOOTFLAG_PAYLOAD_SIZE];
    u16 unknown_key_offsets[BOOTFLAG_MAX_PAYLOAD_KEYS];
    u16 unknown_key_lengths[BOOTFLAG_MAX_PAYLOAD_KEYS];
    u8 calculated_digest[BOOTFLAG_DIGEST_SIZE];
} bootflag_inspection_t;

extern const u8 bootflag_expected_sha1[BOOTFLAG_DIGEST_SIZE];

/* Portable, one-shot SHA-1 implementation used by the generator and verifier. */
void bootflag_sha1(const void *data, size_t length,
                   u8 digest[BOOTFLAG_DIGEST_SIZE]);

/* Generates the fixed PSX2 40 GB image and checks the known test vector. */
int bootflag_generate_standard_40gb(bootflag_image_t *image);

/* Checks header digest, payload digest, and the expected known vector. */
int bootflag_verify_image(const u8 bytes[BOOTFLAG_SIZE],
                          u8 calculated_digest[BOOTFLAG_DIGEST_SIZE]);

/* Parses an exact image without modifying unknown NUL-separated keys. */
void bootflag_inspect_image(const u8 *bytes, size_t size,
                            bootflag_inspection_t *inspection);

const char *bootflag_state_name(bootflag_state_t state);

void bootflag_digest_to_hex(const u8 digest[BOOTFLAG_DIGEST_SIZE],
                            char output[BOOTFLAG_DIGEST_SIZE * 2 + 1]);

#endif
