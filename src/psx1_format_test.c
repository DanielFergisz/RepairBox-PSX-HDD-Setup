#include <string.h>

#include "format_test.h"

#define PFS_ZONE_SIZE 8192u

typedef struct {
    const char *name;
    u32 start;
    u32 size;
} expected_apa_t;

static const expected_apa_t expected_apa[5] = {
    {"__mbr", 0x00000000u, 0x00040000u},
    {"__net", 0x00040000u, 0x00040000u},
    {"__system", 0x00080000u, 0x00080000u},
    {"__sysconf", 0x00100000u, 0x00100000u},
    {"__common", 0x00200000u, 0x00200000u},
};

void format_test_init(format_test_result_t *result)
{
    static const char *const names[FORMAT_TEST_PFS_COUNT] = {
        "__net", "__system", "__sysconf", "__common"
    };
    static const char *const blockdevs[FORMAT_TEST_PFS_COUNT] = {
        "hdd0:__net", "hdd0:__system", "hdd0:__sysconf",
        "hdd0:__common"
    };
    unsigned int index;

    memset(result, 0, sizeof(*result));
    result->failed_step = -1;
    result->apa.name = "APA";
    result->apa.blockdev = "hdd0:";
    for (index = 0; index < FORMAT_TEST_PFS_COUNT; ++index) {
        result->pfs[index].name = names[index];
        result->pfs[index].blockdev = blockdevs[index];
    }
}

int format_test_preflight(const inspector_data_t *scan)
{
    format_test_result_t result;

    format_test_init(&result);
    format_test_analyze_preflight(scan, &result);
    return result.preflight_pass;
}

static int within_nominal(u64 sectors, u64 nominal)
{
    return sectors >= nominal * 3u / 4u &&
           sectors <= nominal * 5u / 4u;
}

static media_capacity_class_t classify_capacity(u64 sectors)
{
    if (within_nominal(sectors, 125000000ULL))
        return MEDIA_CAPACITY_64_GB;
    if (within_nominal(sectors, 250000000ULL))
        return MEDIA_CAPACITY_128_GB;
    if (within_nominal(sectors, 500000000ULL))
        return MEDIA_CAPACITY_256_GB;
    if (within_nominal(sectors, 1000000000ULL))
        return MEDIA_CAPACITY_512_GB;
    return MEDIA_CAPACITY_OTHER;
}

const char *format_test_capacity_name(media_capacity_class_t capacity)
{
    static const char *const names[] = {
        "64 GB", "128 GB", "256 GB", "512 GB", "OTHER"
    };

    if ((unsigned int)capacity >= sizeof(names) / sizeof(names[0]))
        return "OTHER";
    return names[capacity];
}

const char *format_test_state_name(pre_format_state_t state)
{
    static const char *const names[] = {
        "READY_EXISTING_PSX1", "UNFORMATTED",
        "UNKNOWN_PARTITIONING", "INVALID_APA",
        "UNSUPPORTED_PHYSICAL_DEVICE", "TOO_SMALL", "DEVICE_IO_ERROR"
    };

    if ((unsigned int)state >= sizeof(names) / sizeof(names[0]))
        return "DEVICE_IO_ERROR";
    return names[state];
}

const char *format_test_state_reason(pre_format_state_t state)
{
    static const char *const reasons[] = {
        "EXACT_EXISTING_PSX1_APA",
        "PHYSICAL_DEVICE_READABLE_NO_APA",
        "PHYSICAL_DEVICE_READABLE_UNKNOWN_PARTITIONING",
        "PHYSICAL_DEVICE_READABLE_INVALID_APA",
        "REQUIRED_PS2SDK_FORMAT_MODULE_UNUSABLE",
        "PHYSICAL_CAPACITY_BELOW_40GB_SONY_PATH_MINIMUM",
        "HDD_DEVCTL_STATUS_OR_TOTALSECTOR_FAILED"
    };

    if ((unsigned int)state >= sizeof(reasons) / sizeof(reasons[0]))
        return "HDD_DEVCTL_STATUS_OR_TOTALSECTOR_FAILED";
    return reasons[state];
}

void format_test_analyze_preflight(const inspector_data_t *scan,
                                   format_test_result_t *result)
{
    int fileio_usable;
    int hdd_io_usable;
    unsigned int index;

    result->visible_lba28_sector_count =
        scan->hdd_totalsector > 0 ? (u32)scan->hdd_totalsector : 0;
    result->lba48_supported = scan->hdd_islba48 == 1 &&
                              scan->hdd_getmaxlba48 > 0;
    result->lba48_sector_count = result->lba48_supported
        ? (u32)scan->hdd_getmaxlba48 : 0;
    result->physical_sector_count = result->lba48_supported
        ? result->lba48_sector_count : result->visible_lba28_sector_count;
    result->physical_bytes = result->physical_sector_count * 512ULL;
    result->capacity_class = classify_capacity(result->physical_sector_count);

    fileio_usable = scan->filexio_init_result >= 0;
    hdd_io_usable = fileio_usable && scan->hdd_status >= 0 &&
                    scan->hdd_totalsector > 0;
    result->module_usable[0] = fileio_usable;
    result->module_usable[1] = fileio_usable;
    result->module_usable[2] = hdd_io_usable;
    result->module_usable[3] = hdd_io_usable;
    result->module_usable[4] = hdd_io_usable;
    result->module_usable[5] =
        scan->modules[5].module_id >= 0 &&
        scan->modules[5].startup_result >= 0;
    result->usable_module_count = 0;
    for (index = 0; index < FORMAT_TEST_CORE_MODULE_COUNT; ++index)
        result->usable_module_count += result->module_usable[index] != 0;
    result->device_readable = hdd_io_usable;

    if (!result->device_readable) {
        result->pre_format_state = PRE_FORMAT_DEVICE_IO_ERROR;
    } else if (!result->module_usable[5]) {
        result->pre_format_state = PRE_FORMAT_UNSUPPORTED_PHYSICAL_DEVICE;
    } else if (result->physical_sector_count <
               FORMAT_TEST_MIN_VISIBLE_SECTORS) {
        result->pre_format_state = PRE_FORMAT_TOO_SMALL;
    } else if (scan->hdd_formatver == 2 &&
               format_test_exact_apa(scan) &&
               format_test_mbr_valid(scan)) {
        result->pre_format_state = PRE_FORMAT_READY_EXISTING_PSX1;
    } else if (scan->hdd_formatver == 0 && scan->apa_count == 0) {
        result->pre_format_state = PRE_FORMAT_UNFORMATTED;
    } else if (scan->hdd_formatver == 2) {
        result->pre_format_state = PRE_FORMAT_INVALID_APA;
    } else {
        result->pre_format_state = PRE_FORMAT_UNKNOWN_PARTITIONING;
    }

    result->preflight_pass =
        result->pre_format_state == PRE_FORMAT_READY_EXISTING_PSX1 ||
        result->pre_format_state == PRE_FORMAT_UNFORMATTED ||
        result->pre_format_state == PRE_FORMAT_UNKNOWN_PARTITIONING ||
        result->pre_format_state == PRE_FORMAT_INVALID_APA;
    result->capacity_test_valid = result->preflight_pass;
}

int format_test_exact_apa(const inspector_data_t *scan)
{
    unsigned int expected_index;

    if (!scan->hdd_available || scan->apa_overflow ||
        scan->hdd_final_dread_result != 0 || scan->apa_count != 5)
        return 0;
    for (expected_index = 0; expected_index < 5; ++expected_index) {
        unsigned int index;
        unsigned int matches = 0;

        for (index = 0; index < scan->apa_count; ++index) {
            const apa_entry_t *entry = &scan->apa[index];
            if (strcmp(entry->name, expected_apa[expected_index].name) == 0 &&
                entry->start == expected_apa[expected_index].start &&
                entry->size == expected_apa[expected_index].size)
                ++matches;
        }
        if (matches != 1)
            return 0;
    }
    return 1;
}

int format_test_mbr_valid(const inspector_data_t *scan)
{
    static const char sony_magic[] = "Sony Computer Entertainment Inc.";

    return scan->mbr_read_result >= 0 && scan->mbr_magic_valid &&
           scan->mbr_checksum_valid && scan->mbr_version == 2 &&
           scan->mbr_osd_start == 0 && scan->mbr_osd_size == 0 &&
           memcmp(scan->mbr_magic_text, sony_magic,
                  sizeof(sony_magic) - 1u) == 0;
}

int format_test_strict_pfs_valid(const apa_entry_t *entry)
{
    const raw_pfs_diag_t *pfs = &entry->raw_pfs;

    return entry->pfs_applicable && pfs->applicable &&
           pfs->raw_pfs_read_valid && pfs->pfs_super_valid &&
           pfs->pfs_root_valid && pfs->pfs_structure_valid &&
           pfs->version == 3 && pfs->zone_size == PFS_ZONE_SIZE &&
           pfs->declared_subpartitions == 0 &&
           pfs->available_subpartitions == 0 &&
           pfs->subpartition_class == PFS_SUBPART_MATCH &&
           pfs->primary_backup_match &&
           pfs->journal_class == PFS_JOURNAL_CLEAN &&
           pfs->journal_num == 0 && pfs->root_inode_type_valid &&
           pfs->root_directory_valid && pfs->root_directory_dot_found &&
           pfs->root_directory_dotdot_found &&
           !pfs->root_directory_entries_truncated &&
           pfs->root_directory_entries == 2;
}
