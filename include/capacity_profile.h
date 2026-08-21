#ifndef REPAIRBOX_CAPACITY_PROFILE_H
#define REPAIRBOX_CAPACITY_PROFILE_H

#include <tamtypes.h>

#define CAPACITY_NATIVE_256 0x1DD80000u
#define CAPACITY_NATIVE_512_VERIFIED 0x3BB00000u
#define CAPACITY_512_MIN_BYTES 450000000000ULL
#define CAPACITY_512_MAX_BYTES 550000000000ULL
#define CAPACITY_1TB_REFERENCE 0x74706DB0u
#define CAPACITY_LBA28_SECTOR_LIMIT 0x10000000u
#define CAPACITY_ERRNO_U32_FLOOR 0xFFFFF000u

typedef enum capacity_profile {
    CAPACITY_PROFILE_UNSUPPORTED = 0,
    CAPACITY_PROFILE_256_VERIFIED,
    CAPACITY_PROFILE_512_VERIFIED,
    CAPACITY_PROFILE_1TB_VERIFIED,
    CAPACITY_PROFILE_LBA48_UNTESTED
} capacity_profile_t;

capacity_profile_t capacity_profile_detect(u32 native_sectors, int islba48);
const char *capacity_profile_name(capacity_profile_t profile);
int capacity_profile_is_experimental(capacity_profile_t profile);
int capacity_profile_is_supported(capacity_profile_t profile);

#endif
