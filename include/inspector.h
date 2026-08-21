#ifndef REPAIRBOX_PSX1_INSPECTOR_H
#define REPAIRBOX_PSX1_INSPECTOR_H

#include <tamtypes.h>
#include "pfs_ro_diag.h"

#define INSPECTOR_MODULE_COUNT 8
#define INSPECTOR_MAX_APA 128
#define INSPECTOR_PATH_MAX 1024
#define INSPECTOR_HASH_HEX_SIZE 65
#define INSPECTOR_MAX_ROOT_DENTRIES 85
#define INSPECTOR_ROOT_DENTRY_NAME_MAX 256

typedef struct {
    u32 inode_number;
    u32 inode_subpart;
    u32 allocated_length;
    u32 type_bits;
    char name[INSPECTOR_ROOT_DENTRY_NAME_MAX];
} raw_pfs_root_dentry_t;

typedef enum {
    PFS_SUBPART_UNKNOWN = 0,
    PFS_SUBPART_MATCH,
    PFS_SUBPART_MISMATCH,
    PFS_SUBPART_NOT_APPLICABLE
} pfs_subpart_class_t;

typedef enum {
    PFS_JOURNAL_UNKNOWN = 0,
    PFS_JOURNAL_CLEAN,
    PFS_JOURNAL_RECOVERY_REQUIRED,
    PFS_JOURNAL_INVALID
} pfs_journal_class_t;

typedef struct {
    int applicable;
    int apa_header_read_result;
    int apa_header_magic_valid;
    int apa_header_checksum_valid;
    int apa_sub_mapping_valid;
    u32 apa_sub_count;
    char apa_header_sha256[INSPECTOR_HASH_HEX_SIZE];
    int primary_read_result;
    int backup_read_result;
    u32 primary_lba;
    u32 backup_lba;
    char primary_sha256[INSPECTOR_HASH_HEX_SIZE];
    char backup_sha256[INSPECTOR_HASH_HEX_SIZE];
    int primary_backup_match;
    u32 magic;
    u32 version;
    u32 modver;
    u32 fsck_stat;
    u32 zone_size;
    u32 zone_scale;
    u32 declared_subpartitions;
    u32 available_subpartitions;
    pfs_subpart_class_t subpartition_class;
    u64 filesystem_sectors;
    u64 filesystem_zones;
    u32 bitmap_size_sectors;
    u32 bitmap_size_blocks;
    u32 log_number;
    u32 log_subpart;
    u32 log_count;
    u32 root_number;
    u32 root_subpart;
    u32 root_count;
    int descriptor_mapping_valid;
    int journal_read_result;
    u32 journal_lba;
    u32 journal_magic;
    u32 journal_num;
    u32 journal_checksum_stored;
    u32 journal_checksum_calculated;
    int journal_checksum_valid;
    pfs_journal_class_t journal_class;
    char journal_sha256[INSPECTOR_HASH_HEX_SIZE];
    int root_inode_read_result;
    u32 root_inode_lba;
    u32 root_inode_magic;
    u32 root_inode_checksum_stored;
    u32 root_inode_checksum_calculated;
    int root_inode_checksum_valid;
    u32 root_inode_mode;
    u32 root_inode_attr;
    u64 root_inode_size;
    u32 root_inode_number_blocks;
    u32 root_inode_number_data;
    u32 root_inode_number_segdesg;
    u32 root_inode_subpart;
    int root_inode_location_valid;
    int root_inode_type_valid;
    int root_inode_extents_valid;
    char root_inode_sha256[INSPECTOR_HASH_HEX_SIZE];
    int root_directory_read_result;
    u32 root_directory_lba;
    u32 root_directory_entries;
    u32 root_directory_entries_stored;
    int root_directory_entries_truncated;
    raw_pfs_root_dentry_t
        root_directory_entry[INSPECTOR_MAX_ROOT_DENTRIES];
    int root_directory_dot_found;
    int root_directory_dotdot_found;
    int root_directory_valid;
    char root_directory_sha256[INSPECTOR_HASH_HEX_SIZE];
    int bitmap_read_result;
    u32 bitmap_lba;
    u32 bitmap_first_word;
    int bitmap_reserved_bit0_set;
    int bitmap_structure_valid;
    char bitmap_sha256[INSPECTOR_HASH_HEX_SIZE];
    int raw_pfs_read_valid;
    int pfs_super_valid;
    int pfs_root_valid;
    int pfs_structure_valid;
} raw_pfs_diag_t;

typedef struct module_result {
    const char *name;
    int module_id;
    int startup_result;
} module_result_t;

typedef struct apa_entry {
    char name[256];
    u32 start;
    u32 size;
    u32 hisize;
    u32 mode;
    u32 attr;
    u32 private_fields[6];
    int pfs_applicable;
    int pfs_mount_result;
    int pfs_diag_result;
    pfs_ro_mount_diag_t pfs_diag;
    int pfs_zone_size;
    int pfs_zone_free;
    int pfs_dopen_result;
    int pfs_final_dread_result;
    int pfs_dclose_result;
    int pfs_umount_result;
    unsigned int directory_count;
    unsigned int file_count;
    u64 byte_count;
    raw_pfs_diag_t raw_pfs;
} apa_entry_t;

typedef struct optional_hash {
    const char *label;
    u32 first_sector;
    u32 last_sector;
    int read_result;
    char sha256[65];
} optional_hash_t;

typedef struct inspector_data {
    int lmb_patch_result;
    int prefix_patch_result;
    int filexio_init_result;
    module_result_t modules[INSPECTOR_MODULE_COUNT];
    char romver[32];
    int romver_open_result;
    int romver_read_result;
    int cdvd_init_result;
    int cdvd_disk_type;
    int cdvd_status;
    int hdd_status;
    int hdd_formatver;
    int hdd_totalsector;
    int hdd_maxsector;
    int hdd_getmaxlba48;
    int hdd_islba48;
    int hdd_dopen_result;
    int hdd_final_dread_result;
    int hdd_dclose_result;
    int hdd_available;
    int apa_overflow;
    unsigned int apa_count;
    apa_entry_t apa[INSPECTOR_MAX_APA];
    int mbr_read_result;
    int mbr_magic_valid;
    int mbr_checksum_valid;
    u32 mbr_checksum_stored;
    u32 mbr_checksum_calculated;
    u32 mbr_magic;
    u32 mbr_version;
    u32 mbr_nsector;
    u32 mbr_osd_start;
    u32 mbr_osd_size;
    u32 mbr_modver;
    char mbr_magic_text[33];
    char sector0_sha256[65];
    optional_hash_t optional_hashes[3];
    unsigned int pfs_mountable_count;
    unsigned int pfs_total_directories;
    unsigned int pfs_total_files;
    u64 pfs_total_bytes;
    int pfs_discovery_valid;
    int pfs_scan_completed;
    int raw_pfs_target_count;
    int raw_pfs_found_count;
    int raw_pfs_valid_count;
    int raw_pfs_discovery_valid;
    int dvr_dopen_result;
    int dvr_final_dread_result;
    int dvr_dclose_result;
    unsigned int dvr_entry_count;
    int xfrom_attempted;
    int xfrom_result;
} inspector_data_t;

void inspector_initialize(inspector_data_t *data);
void inspector_scan_layout(inspector_data_t *data);
void inspector_scan(inspector_data_t *data);
const char *inspector_pfs_diag_reason_name(u32 reason);
const char *inspector_pfs_diag_stage_name(u32 stage);
const char *inspector_subpartition_class_name(pfs_subpart_class_t value);
const char *inspector_journal_class_name(pfs_journal_class_t value);

#endif
