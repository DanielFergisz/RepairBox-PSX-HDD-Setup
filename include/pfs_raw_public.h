#ifndef PFS_RAW_PUBLIC_H
#define PFS_RAW_PUBLIC_H

/* Read-only mirror of public PS2SDK libpfs.h on-disk structures. */
#include <tamtypes.h>

#define PFS_RAW_BLOCK_SIZE              0x2000u
#define PFS_RAW_SUPER_MAGIC             0x50465300u
#define PFS_RAW_JOURNAL_MAGIC           0x5046534cu
#define PFS_RAW_SEGD_MAGIC              0x53454744u
#define PFS_RAW_SEGI_MAGIC              0x53454749u
#define PFS_RAW_FORMAT_VERSION          3u
#define PFS_RAW_MAX_SUBPARTS            64u
#define PFS_RAW_INODE_MAX_BLOCKS        114u
#define PFS_RAW_SUPER_SECTOR            8192u
#define PFS_RAW_SUPER_BACKUP_SECTOR     8193u
#define PFS_RAW_FSCK_OK                  0u
#define PFS_RAW_FSCK_WRITE_ERROR         1u
#define PFS_RAW_FSCK_ERRORS_FIXED        2u

typedef struct {
    u32 number;
    u16 subpart;
    u16 count;
} pfs_raw_blockinfo_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 modver;
    u32 fsck_stat;
    u32 zone_size;
    u32 num_subs;
    pfs_raw_blockinfo_t log;
    pfs_raw_blockinfo_t root;
} pfs_raw_super_t;

typedef struct {
    u32 sector;
    u16 sub;
    u16 log_sector;
} pfs_raw_journal_log_t;

typedef struct {
    u32 magic;
    u16 num;
    u16 checksum;
    pfs_raw_journal_log_t log[127];
} pfs_raw_journal_t;

typedef struct {
    u16 unused;
    u8 sec;
    u8 min;
    u8 hour;
    u8 day;
    u8 month;
    u8 year;
} pfs_raw_datetime_t;

typedef struct {
    u32 checksum;
    u32 magic;
    pfs_raw_blockinfo_t inode_block;
    pfs_raw_blockinfo_t next_segment;
    pfs_raw_blockinfo_t last_segment;
    pfs_raw_blockinfo_t unused;
    pfs_raw_blockinfo_t data[PFS_RAW_INODE_MAX_BLOCKS];
    u16 mode;
    u16 attr;
    u16 uid;
    u16 gid;
    pfs_raw_datetime_t atime;
    pfs_raw_datetime_t ctime;
    pfs_raw_datetime_t mtime;
    u64 size;
    u32 number_blocks;
    u32 number_data;
    u32 number_segdesg;
    u32 subpart;
    u32 reserved[4];
} pfs_raw_inode_t;

typedef struct {
    u32 inode;
    u8 sub;
    u8 path_len;
    u16 allocated_len;
    char path[504];
} pfs_raw_dentry_t;

_Static_assert(sizeof(pfs_raw_blockinfo_t) == 8, "PS2SDK PFS blockinfo ABI");
_Static_assert(sizeof(pfs_raw_super_t) == 40, "PS2SDK PFS superblock ABI");
_Static_assert(sizeof(pfs_raw_journal_t) == 1024, "PS2SDK PFS journal ABI");
_Static_assert(sizeof(pfs_raw_inode_t) == 1024, "PS2SDK PFS inode ABI");
_Static_assert(sizeof(pfs_raw_dentry_t) == 512, "PS2SDK PFS dentry ABI");

#endif
