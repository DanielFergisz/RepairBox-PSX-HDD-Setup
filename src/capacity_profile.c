#include "capacity_profile.h"

capacity_profile_t capacity_profile_detect(u32 native_sectors, int islba48)
{
    u64 bytes = (u64)native_sectors * 512u;

    if (islba48 != 1 || native_sectors <= CAPACITY_LBA28_SECTOR_LIMIT ||
        native_sectors >= CAPACITY_ERRNO_U32_FLOOR)
        return CAPACITY_PROFILE_UNSUPPORTED;
    if (native_sectors == CAPACITY_NATIVE_256)
        return CAPACITY_PROFILE_256_VERIFIED;
    if (bytes >= CAPACITY_512_MIN_BYTES && bytes <= CAPACITY_512_MAX_BYTES)
        return CAPACITY_PROFILE_512_VERIFIED;
    if (bytes >= 900000000000ULL && bytes <= 1099000000000ULL)
        return CAPACITY_PROFILE_1TB_VERIFIED;
    return CAPACITY_PROFILE_LBA48_UNTESTED;
}

const char *capacity_profile_name(capacity_profile_t profile)
{
    switch (profile) {
        case CAPACITY_PROFILE_256_VERIFIED:
            return "256 GB";
        case CAPACITY_PROFILE_512_VERIFIED:
            return "512 GB";
        case CAPACITY_PROFILE_1TB_VERIFIED:
            return "1 TB";
        case CAPACITY_PROFILE_LBA48_UNTESTED:
            return "LBA48 MEDIA";
        default:
            return "UNSUPPORTED MEDIA";
    }
}

int capacity_profile_is_experimental(capacity_profile_t profile)
{
    return profile == CAPACITY_PROFILE_LBA48_UNTESTED;
}

int capacity_profile_is_supported(capacity_profile_t profile)
{
    return profile != CAPACITY_PROFILE_UNSUPPORTED;
}
