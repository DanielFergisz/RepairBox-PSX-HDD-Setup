#ifndef REPAIRBOX_PFS_RO_DIAG_H
#define REPAIRBOX_PFS_RO_DIAG_H

#include <tamtypes.h>

/* Private, read-only diagnostic command exposed by the bundled PFS fork. */
#define PFS_RO_DIAG_DEVCTL 0x7F01
#define PFS_RO_DIAG_VERSION 1

enum pfs_ro_diag_stage {
    PFS_RO_DIAG_STAGE_RESET = 0,
    PFS_RO_DIAG_STAGE_SUPERBLOCK = 1,
    PFS_RO_DIAG_STAGE_JOURNAL = 2,
    PFS_RO_DIAG_STAGE_BITMAP = 3,
    PFS_RO_DIAG_STAGE_COMPLETE = 4
};

enum pfs_ro_diag_reason {
    PFS_RO_DIAG_REASON_NONE = 0,
    PFS_RO_DIAG_CACHE_ALLOC_FAILED = 1,
    PFS_RO_DIAG_SUPERBLOCK_READ_FAILED = 2,
    PFS_RO_DIAG_SUPERBLOCK_MAGIC_INVALID = 3,
    PFS_RO_DIAG_SUPERBLOCK_VERSION_UNSUPPORTED = 4,
    PFS_RO_DIAG_ZONE_SIZE_INVALID = 5,
    PFS_RO_DIAG_FSCK_WRITE_ERROR = 6,
    PFS_RO_DIAG_FILESYSTEM_LARGER_THAN_PARTITION = 7,
    PFS_RO_DIAG_SUBPARTITIONS_REQUIRE_UPDATE = 8,
    PFS_RO_DIAG_JOURNAL_READ_FAILED = 9,
    PFS_RO_DIAG_JOURNAL_MAGIC_INVALID = 10,
    PFS_RO_DIAG_JOURNAL_CHECKSUM_INVALID = 11,
    PFS_RO_DIAG_JOURNAL_DIRTY = 12,
    PFS_RO_DIAG_BITMAP_SCAN_FAILED = 13,
    PFS_RO_DIAG_MOUNT_OK = 14,
    PFS_RO_DIAG_NOT_APPLICABLE = 15
};

typedef struct pfs_ro_mount_diag {
    u32 version;
    int mount_result;
    u32 stage;
    u32 reason;
    u32 actual_subs;
    int super_read_result;
    u32 super_magic;
    u32 super_version;
    u32 super_modver;
    u32 super_fsck_stat;
    u32 super_zone_size;
    u32 super_num_subs;
    u32 super_log_number;
    u16 super_log_subpart;
    u16 super_log_count;
    u32 super_root_number;
    u16 super_root_subpart;
    u16 super_root_count;
    int journal_read_result;
    u32 journal_magic;
    u32 journal_num;
    u32 journal_checksum_stored;
    u32 journal_checksum_calculated;
    int bitmap_result;
} pfs_ro_mount_diag_t;

#endif
