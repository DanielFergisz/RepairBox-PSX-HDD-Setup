#ifndef REPAIRBOX_DIRECT_READY40_BOOTSTRAP_H
#define REPAIRBOX_DIRECT_READY40_BOOTSTRAP_H

#include <tamtypes.h>

#include "source_media.h"

#define BOOTSTRAP_ROOT source_media_bootstrap_root()
#define BOOTSTRAP_BIN_PATH source_media_bootstrap_bin_path()
#define BOOTSTRAP_SUMS_PATH source_media_bootstrap_sums_path()
#define BOOTSTRAP_SECTOR_COUNT 0x27E7u
#define BOOTSTRAP_BYTE_COUNT 0x004FCE00u
#define BOOTSTRAP_SHA256_SIZE 32u

typedef struct bootstrap_result {
    int validation_attempted;
    int binary_open_result;
    int binary_final_read_result;
    int binary_close_result;
    u32 binary_size;
    int sums_open_result;
    int sums_read_result;
    int sums_close_result;
    int sums_parse_result;
    int size_valid;
    int sha256_valid;
    int validation_valid;
    unsigned char expected_sha256[BOOTSTRAP_SHA256_SIZE];
    unsigned char source_sha256[BOOTSTRAP_SHA256_SIZE];
    int write_attempted;
    int write_return;
    u32 written_sectors;
    u32 write_duration_ms;
    int readback_return;
    u32 readback_sectors;
    int readback_sha256_valid;
    int sample_sectors_valid;
    int bootstrap_valid;
    u32 failure_lba;
    u32 failure_requested_bytes;
    int failure_actual_bytes;
} bootstrap_result_t;

void bootstrap_initialize(bootstrap_result_t *result);
void bootstrap_validate_usb(bootstrap_result_t *result);
int bootstrap_write_and_verify(bootstrap_result_t *result);

#endif
