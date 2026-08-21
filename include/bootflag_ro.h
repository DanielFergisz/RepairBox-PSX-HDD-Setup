#ifndef REPAIRBOX_DIRECT_READY40_BOOTFLAG_RO_H
#define REPAIRBOX_DIRECT_READY40_BOOTFLAG_RO_H

#include <tamtypes.h>

#define BOOTFLAG_RO_SIZE 512
#define BOOTFLAG_RO_DIGEST_SIZE 20
#define BOOTFLAG_RO_PAYLOAD_OFFSET 0x14
#define BOOTFLAG_RO_PAYLOAD_SIZE (BOOTFLAG_RO_SIZE - BOOTFLAG_RO_PAYLOAD_OFFSET)
#define BOOTFLAG_RO_XFROM_PATH "xfrom0:/BIEXEC-SYSTEM/bootflag.txt"

typedef enum bootflag_ro_state {
    BOOTFLAG_RO_NORMAL = 0,
    BOOTFLAG_RO_PENDING_40GB,
    BOOTFLAG_RO_INVALID,
} bootflag_ro_state_t;

typedef struct bootflag_ro_result {
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
    u32 unknown_key_count;
    bootflag_ro_state_t state;
    int pending_40gb_preserved;
    u8 calculated_digest[BOOTFLAG_RO_DIGEST_SIZE];
} bootflag_ro_result_t;

void bootflag_ro_allow_pending_reinstall(int allow);
void bootflag_ro_inspect(bootflag_ro_result_t *result);
const char *bootflag_ro_state_name(bootflag_ro_state_t state);

#endif
