#include <errno.h>
#include <fcntl.h>
#include <hdd-ioctl.h>
#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <libcdvd.h>
#include <stdio.h>
#include <string.h>

#include <fileXio_rpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "inspector.h"
#include "pfs_raw_public.h"
#include "sha256.h"

static int is_system_pfs_name(const char *name)
{
    return strcmp(name, "__net") == 0 ||
           strcmp(name, "__system") == 0 ||
           strcmp(name, "__sysconf") == 0 ||
           strcmp(name, "__common") == 0;
}

const char *inspector_subpartition_class_name(pfs_subpart_class_t value)
{
    switch (value) {
    case PFS_SUBPART_MATCH: return "MATCH";
    case PFS_SUBPART_MISMATCH: return "MISMATCH";
    case PFS_SUBPART_NOT_APPLICABLE: return "NOT_APPLICABLE";
    default: return "UNKNOWN";
    }
}

const char *inspector_journal_class_name(pfs_journal_class_t value)
{
    switch (value) {
    case PFS_JOURNAL_CLEAN: return "CLEAN";
    case PFS_JOURNAL_RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    case PFS_JOURNAL_INVALID: return "INVALID";
    default: return "UNKNOWN";
    }
}

const char *inspector_pfs_diag_stage_name(u32 stage)
{
    switch (stage) {
    case PFS_RO_DIAG_STAGE_RESET: return "RESET";
    case PFS_RO_DIAG_STAGE_SUPERBLOCK: return "SUPERBLOCK";
    case PFS_RO_DIAG_STAGE_JOURNAL: return "JOURNAL";
    case PFS_RO_DIAG_STAGE_BITMAP: return "BITMAP";
    case PFS_RO_DIAG_STAGE_COMPLETE: return "COMPLETE";
    default: return "UNKNOWN";
    }
}

const char *inspector_pfs_diag_reason_name(u32 reason)
{
    switch (reason) {
    case PFS_RO_DIAG_REASON_NONE: return "NONE";
    case PFS_RO_DIAG_CACHE_ALLOC_FAILED: return "CACHE_ALLOC_FAILED";
    case PFS_RO_DIAG_SUPERBLOCK_READ_FAILED: return "SUPERBLOCK_READ_FAILED";
    case PFS_RO_DIAG_SUPERBLOCK_MAGIC_INVALID: return "SUPERBLOCK_MAGIC_INVALID";
    case PFS_RO_DIAG_SUPERBLOCK_VERSION_UNSUPPORTED: return "SUPERBLOCK_VERSION_UNSUPPORTED";
    case PFS_RO_DIAG_ZONE_SIZE_INVALID: return "ZONE_SIZE_INVALID";
    case PFS_RO_DIAG_FSCK_WRITE_ERROR: return "FSCK_WRITE_ERROR";
    case PFS_RO_DIAG_FILESYSTEM_LARGER_THAN_PARTITION: return "FILESYSTEM_LARGER_THAN_PARTITION";
    case PFS_RO_DIAG_SUBPARTITIONS_REQUIRE_UPDATE: return "SUBPARTITIONS_REQUIRE_UPDATE";
    case PFS_RO_DIAG_JOURNAL_READ_FAILED: return "JOURNAL_READ_FAILED";
    case PFS_RO_DIAG_JOURNAL_MAGIC_INVALID: return "JOURNAL_MAGIC_INVALID";
    case PFS_RO_DIAG_JOURNAL_CHECKSUM_INVALID: return "JOURNAL_CHECKSUM_INVALID";
    case PFS_RO_DIAG_JOURNAL_DIRTY: return "JOURNAL_DIRTY";
    case PFS_RO_DIAG_BITMAP_SCAN_FAILED: return "BITMAP_SCAN_FAILED";
    case PFS_RO_DIAG_MOUNT_OK: return "MOUNT_OK";
    case PFS_RO_DIAG_NOT_APPLICABLE: return "NOT_APPLICABLE";
    default: return "UNKNOWN";
    }
}
#define APA_MAGIC 0x00415041u
#define APA_HEADER_BYTES 1024u
#define SECTOR_BYTES 512u

extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[] __attribute__((aligned(16)));
extern unsigned int size_fileXio_irx;
extern unsigned char ps2dev9_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_psx1_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2hdd_psx1_irx;
extern unsigned char ps2fs_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2fs_irx;
extern unsigned char usbd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbhdfsd_irx;

typedef struct embedded_module {
    const char *name;
    unsigned char *data;
    unsigned int *size;
    unsigned int argument_length;
    const char *arguments;
} embedded_module_t;

typedef struct apa_time {
    u8 unused, sec, min, hour, day, month;
    u16 year;
} apa_time_t;

typedef struct apa_sub {
    u32 start;
    u32 length;
} apa_sub_t;

typedef struct apa_header_public {
    u32 checksum, magic, next, prev;
    char id[32], rpwd[8], fpwd[8];
    u32 start, length;
    u16 type, flags;
    u32 nsub;
    apa_time_t created;
    u32 main, number, modver, padding1[7];
    char padding2[128];
    struct {
        char magic[32];
        u32 version, nsector;
        apa_time_t created;
        u32 osd_start, osd_size;
        char padding3[200];
    } mbr;
    apa_sub_t subs[64];
} apa_header_public_t;

_Static_assert(sizeof(apa_header_public_t) == APA_HEADER_BYTES,
               "public APA header layout must be 1024 bytes");

static const char hdd_arguments[] = "-o\0" "4\0" "-n\0" "20";
static const char pfs_arguments[] =
    "-m\0" "4\0" "-o\0" "10\0" "-n\0" "40";

static const embedded_module_t modules[INSPECTOR_MODULE_COUNT] = {
    {"iomanX", iomanX_irx, &size_iomanX_irx, 0, NULL},
    {"fileXio", fileXio_irx, &size_fileXio_irx, 0, NULL},
    {"ps2dev9", ps2dev9_irx, &size_ps2dev9_irx, 0, NULL},
    {"ps2atad", ps2atad_irx, &size_ps2atad_irx, 0, NULL},
    {"ps2hdd-psx1-sparse-skip", ps2hdd_psx1_irx,
     &size_ps2hdd_psx1_irx,
     sizeof(hdd_arguments), hdd_arguments},
    {"ps2fs", ps2fs_irx, &size_ps2fs_irx,
     sizeof(pfs_arguments), pfs_arguments},
    {"usbd", usbd_irx, &size_usbd_irx, 0, NULL},
    {"usbhdfsd", usbhdfsd_irx, &size_usbhdfsd_irx, 0, NULL},
};

static unsigned char sector_buffer[APA_HEADER_BYTES] __attribute__((aligned(64)));

static void load_module(inspector_data_t *data, unsigned int index)
{
    int result = 0x7fffffff;
    const embedded_module_t *module = &modules[index];

    data->modules[index].name = module->name;
    data->modules[index].module_id =
        SifExecModuleBuffer(module->data, *module->size,
                            module->argument_length, module->arguments,
                            &result);
    data->modules[index].startup_result = result;
}

static int read_sectors(u32 lba, u32 count, void *output)
{
    hddAtaTransfer_t request;

    request.lba = lba;
    request.size = count;
    return fileXioDevctl("hdd0:", HDIOC_READSECTOR,
                         &request, sizeof(request), output,
                         count * SECTOR_BYTES);
}

static void read_romver(inspector_data_t *data)
{
    int fd;

    memset(data->romver, 0, sizeof(data->romver));
    fd = fileXioOpen("rom0:ROMVER", O_RDONLY, 0);
    data->romver_open_result = fd;
    if (fd < 0) {
        data->romver_read_result = fd;
        return;
    }
    data->romver_read_result =
        fileXioRead(fd, data->romver, sizeof(data->romver) - 1);
    fileXioClose(fd);
}

static void enumerate_hdd(inspector_data_t *data)
{
    iox_dirent_t dirent;
    int fd = fileXioDopen("hdd0:");
    int result = fd;

    data->hdd_dopen_result = fd;
    data->hdd_available = fd >= 0;
    if (fd < 0) {
        data->hdd_final_dread_result = fd;
        return;
    }
    for (;;) {
        memset(&dirent, 0, sizeof(dirent));
        result = fileXioDread(fd, &dirent);
        if (result <= 0)
            break;
        if (data->apa_count >= INSPECTOR_MAX_APA) {
            data->apa_overflow = 1;
            continue;
        }
        {
            apa_entry_t *entry = &data->apa[data->apa_count++];
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->name, dirent.name, sizeof(entry->name));
            entry->name[sizeof(entry->name) - 1] = '\0';
            entry->mode = dirent.stat.mode;
            entry->attr = dirent.stat.attr;
            entry->size = dirent.stat.size;
            entry->hisize = dirent.stat.hisize;
            entry->private_fields[0] = dirent.stat.private_0;
            entry->private_fields[1] = dirent.stat.private_1;
            entry->private_fields[2] = dirent.stat.private_2;
            entry->private_fields[3] = dirent.stat.private_3;
            entry->private_fields[4] = dirent.stat.private_4;
            entry->private_fields[5] = dirent.stat.private_5;
            entry->start = dirent.stat.private_5;
            entry->pfs_applicable = is_system_pfs_name(entry->name);
            entry->pfs_mount_result = -ENODEV;
            entry->pfs_diag_result = -ENODEV;
            entry->pfs_diag.version = PFS_RO_DIAG_VERSION;
            entry->pfs_diag.mount_result = -ENODEV;
            entry->pfs_diag.stage = PFS_RO_DIAG_STAGE_RESET;
            entry->pfs_diag.reason = entry->pfs_applicable
                                         ? PFS_RO_DIAG_REASON_NONE
                                         : PFS_RO_DIAG_NOT_APPLICABLE;
            entry->pfs_zone_size = -ENODEV;
            entry->pfs_zone_free = -ENODEV;
            entry->pfs_dopen_result = -ENODEV;
            entry->pfs_final_dread_result = -ENODEV;
            entry->pfs_dclose_result = -ENODEV;
            entry->pfs_umount_result = -ENODEV;
        }
    }
    data->hdd_final_dread_result = result;
    data->hdd_dclose_result = fileXioDclose(fd);
}

static void parse_mbr(inspector_data_t *data)
{
    apa_header_public_t *header = (apa_header_public_t *)sector_buffer;
    u32 *words = (u32 *)sector_buffer;
    u32 checksum = 0;
    unsigned int index;
    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_context_t hash;

    data->mbr_read_result = read_sectors(0, 2, sector_buffer);
    if (data->mbr_read_result < 0)
        return;
    for (index = 1; index < 256; ++index)
        checksum += words[index];
    data->mbr_checksum_stored = header->checksum;
    data->mbr_checksum_calculated = checksum;
    data->mbr_checksum_valid = checksum == header->checksum;
    data->mbr_magic = header->magic;
    data->mbr_magic_valid = header->magic == APA_MAGIC;
    data->mbr_modver = header->modver;
    data->mbr_version = header->mbr.version;
    data->mbr_nsector = header->mbr.nsector;
    data->mbr_osd_start = header->mbr.osd_start;
    data->mbr_osd_size = header->mbr.osd_size;
    memcpy(data->mbr_magic_text, header->mbr.magic, 32);
    data->mbr_magic_text[32] = '\0';
    sha256_init(&hash);
    sha256_update(&hash, sector_buffer, SECTOR_BYTES);
    sha256_final(&hash, digest);
    sha256_to_hex(digest, data->sector0_sha256);
}

static void hash_sector_range(optional_hash_t *result)
{
    sha256_context_t hash;
    unsigned char digest[SHA256_DIGEST_SIZE];
    u32 sector;

    sha256_init(&hash);
    result->read_result = 0;
    for (sector = result->first_sector; sector <= result->last_sector;
         ++sector) {
        int read_result = read_sectors(sector, 1, sector_buffer);
        if (read_result < 0) {
            result->read_result = read_result;
            result->sha256[0] = '\0';
            return;
        }
        sha256_update(&hash, sector_buffer, SECTOR_BYTES);
    }
    sha256_final(&hash, digest);
    sha256_to_hex(digest, result->sha256);
}

static void hash_buffer(const void *buffer, size_t size,
                        char output[INSPECTOR_HASH_HEX_SIZE])
{
    sha256_context_t hash;
    unsigned char digest[SHA256_DIGEST_SIZE];

    sha256_init(&hash);
    sha256_update(&hash, buffer, size);
    sha256_final(&hash, digest);
    sha256_to_hex(digest, output);
}

static u32 checksum_words(const void *buffer, unsigned int first_word)
{
    const u32 *words = (const u32 *)buffer;
    u32 checksum = 0;
    unsigned int index;

    for (index = first_word; index < APA_HEADER_BYTES / sizeof(u32); ++index)
        checksum += words[index];
    return checksum;
}

static int is_power_of_two(u32 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static u32 log2_u32(u32 value)
{
    u32 result = 0;

    while (value > 1) {
        value >>= 1;
        result++;
    }
    return result;
}

/* Exact public PS2SDK pfsGetBitmapSizeSectors() arithmetic. */
static u32 pfs_bitmap_size_sectors(u32 zone_scale, u32 partition_sectors)
{
    u32 remainder;
    u32 zones = partition_sectors / (1u << zone_scale);

    remainder = zones & 7u;
    zones = zones / 8u + remainder;
    remainder = zones & 511u;
    return zones / 512u + remainder;
}

static int map_blockinfo(const apa_header_public_t *header,
                         const apa_entry_t *entry,
                         const pfs_raw_blockinfo_t *block,
                         u32 zone_scale, u32 sectors,
                         u32 disk_sectors, u32 *absolute_lba)
{
    u32 base;
    u32 length;
    u64 offset;
    u64 end;

    if (block->subpart == 0) {
        base = entry->start;
        length = entry->size;
    } else {
        u32 sub_index = (u32)block->subpart - 1u;
        if (sub_index >= header->nsub || sub_index >= PFS_RAW_MAX_SUBPARTS)
            return 0;
        base = header->subs[sub_index].start;
        length = header->subs[sub_index].length;
    }
    offset = (u64)block->number << zone_scale;
    end = offset + sectors;
    if (end > length || (u64)base + end > disk_sectors)
        return 0;
    *absolute_lba = base + (u32)offset;
    return 1;
}

static int validate_apa_sub_mapping(const apa_header_public_t *header,
                                    u32 disk_sectors)
{
    u32 index;

    if (header->nsub > PFS_RAW_MAX_SUBPARTS)
        return 0;
    for (index = 0; index < header->nsub; ++index) {
        u64 end = (u64)header->subs[index].start +
                  header->subs[index].length;
        if (header->subs[index].length == 0 || end > disk_sectors)
            return 0;
    }
    return 1;
}

static int validate_root_directory(const unsigned char *buffer,
                                   raw_pfs_diag_t *diag)
{
    unsigned int offset = 0;
    int syntax_valid = 1;

    while (offset + 8u <= APA_HEADER_BYTES) {
        const pfs_raw_dentry_t *dentry =
            (const pfs_raw_dentry_t *)(buffer + offset);
        u32 allocated = dentry->allocated_len & 0x0fffu;
        u32 minimum = ((u32)dentry->path_len + 8u + 3u) & ~3u;

        if (allocated == 0)
            break;
        if ((allocated & 3u) != 0 || allocated < minimum) {
            syntax_valid = 0;
            break;
        }
        if (offset + allocated > APA_HEADER_BYTES)
            break; /* Valid entry continues beyond the first sampled block. */
        if (dentry->path_len > 0) {
            u32 active_index = diag->root_directory_entries;

            diag->root_directory_entries++;
            if (active_index < INSPECTOR_MAX_ROOT_DENTRIES) {
                raw_pfs_root_dentry_t *stored =
                    &diag->root_directory_entry[active_index];

                stored->inode_number = dentry->inode;
                stored->inode_subpart = dentry->sub;
                stored->allocated_length = allocated;
                stored->type_bits = dentry->allocated_len & FIO_S_IFMT;
                memcpy(stored->name, dentry->path, dentry->path_len);
                stored->name[dentry->path_len] = '\0';
                diag->root_directory_entries_stored++;
            } else {
                diag->root_directory_entries_truncated = 1;
            }
            if (dentry->path_len == 1 && dentry->path[0] == '.')
                diag->root_directory_dot_found = 1;
            if (dentry->path_len == 2 && dentry->path[0] == '.' &&
                dentry->path[1] == '.')
                diag->root_directory_dotdot_found = 1;
        }
        offset += allocated;
    }
    return syntax_valid && diag->root_directory_dot_found &&
           diag->root_directory_dotdot_found;
}

static void inspect_raw_pfs(inspector_data_t *data, apa_entry_t *entry)
{
    raw_pfs_diag_t *diag = &entry->raw_pfs;
    unsigned char apa_raw[APA_HEADER_BYTES] __attribute__((aligned(64)));
    unsigned char primary[SECTOR_BYTES] __attribute__((aligned(64)));
    unsigned char backup[SECTOR_BYTES] __attribute__((aligned(64)));
    unsigned char component[APA_HEADER_BYTES] __attribute__((aligned(64)));
    const apa_header_public_t *header = (const apa_header_public_t *)apa_raw;
    const pfs_raw_super_t *super = (const pfs_raw_super_t *)primary;
    const pfs_raw_journal_t *journal;
    const pfs_raw_inode_t *inode;
    u32 disk_sectors = data->hdd_totalsector > 0
                           ? (u32)data->hdd_totalsector : 0;
    u32 calculated;
    u32 index;
    int zone_valid;
    int journal_mapping = 0;
    int root_mapping = 0;

    memset(diag, 0, sizeof(*diag));
    diag->applicable = is_system_pfs_name(entry->name);
    diag->apa_header_read_result = -ENODEV;
    diag->primary_read_result = -ENODEV;
    diag->backup_read_result = -ENODEV;
    diag->journal_read_result = -ENODEV;
    diag->root_inode_read_result = -ENODEV;
    diag->root_directory_read_result = -ENODEV;
    diag->bitmap_read_result = -ENODEV;
    diag->subpartition_class = diag->applicable
                                   ? PFS_SUBPART_UNKNOWN
                                   : PFS_SUBPART_NOT_APPLICABLE;
    diag->journal_class = PFS_JOURNAL_UNKNOWN;
    if (!diag->applicable)
        return;

    diag->apa_header_read_result = read_sectors(entry->start, 2, apa_raw);
    if (diag->apa_header_read_result < 0)
        return;
    hash_buffer(apa_raw, sizeof(apa_raw), diag->apa_header_sha256);
    diag->apa_header_magic_valid = header->magic == APA_MAGIC;
    calculated = checksum_words(apa_raw, 1);
    diag->apa_header_checksum_valid = calculated == header->checksum;
    diag->apa_sub_count = header->nsub;
    diag->available_subpartitions = header->nsub;
    diag->apa_sub_mapping_valid =
        validate_apa_sub_mapping(header, disk_sectors);

    diag->primary_lba = entry->start + PFS_RAW_SUPER_SECTOR;
    diag->backup_lba = entry->start + PFS_RAW_SUPER_BACKUP_SECTOR;
    if ((u64)PFS_RAW_SUPER_BACKUP_SECTOR < entry->size) {
        diag->primary_read_result = read_sectors(diag->primary_lba, 1, primary);
        diag->backup_read_result = read_sectors(diag->backup_lba, 1, backup);
    }
    if (diag->primary_read_result < 0 || diag->backup_read_result < 0)
        return;
    hash_buffer(primary, sizeof(primary), diag->primary_sha256);
    hash_buffer(backup, sizeof(backup), diag->backup_sha256);
    diag->primary_backup_match =
        memcmp(primary, backup, sizeof(pfs_raw_super_t)) == 0;
    diag->magic = super->magic;
    diag->version = super->version;
    diag->modver = super->modver;
    diag->fsck_stat = super->fsck_stat;
    diag->zone_size = super->zone_size;
    diag->declared_subpartitions = super->num_subs;
    diag->log_number = super->log.number;
    diag->log_subpart = super->log.subpart;
    diag->log_count = super->log.count;
    diag->root_number = super->root.number;
    diag->root_subpart = super->root.subpart;
    diag->root_count = super->root.count;
    diag->subpartition_class =
        super->num_subs == header->nsub ? PFS_SUBPART_MATCH
                                       : PFS_SUBPART_MISMATCH;

    zone_valid = is_power_of_two(super->zone_size) &&
                 super->zone_size >= 2048u &&
                 super->zone_size <= 131072u &&
                 super->zone_size % SECTOR_BYTES == 0;
    if (zone_valid) {
        u32 total_bitmap_sectors = 0;
        u32 sectors_per_zone = super->zone_size / SECTOR_BYTES;
        diag->zone_scale = log2_u32(sectors_per_zone);
        diag->filesystem_sectors = entry->size;
        total_bitmap_sectors =
            pfs_bitmap_size_sectors(diag->zone_scale, entry->size);
        for (index = 0; index < header->nsub &&
                        index < PFS_RAW_MAX_SUBPARTS; ++index) {
            diag->filesystem_sectors += header->subs[index].length;
            total_bitmap_sectors += pfs_bitmap_size_sectors(
                diag->zone_scale, header->subs[index].length);
        }
        diag->filesystem_zones =
            diag->filesystem_sectors >> diag->zone_scale;
        diag->bitmap_size_sectors = total_bitmap_sectors;
        diag->bitmap_size_blocks =
            (total_bitmap_sectors + sectors_per_zone - 1u) /
            sectors_per_zone;
        journal_mapping = map_blockinfo(header, entry, &super->log,
                                        diag->zone_scale, 2,
                                        disk_sectors, &diag->journal_lba);
        root_mapping = map_blockinfo(header, entry, &super->root,
                                     diag->zone_scale, 2,
                                     disk_sectors, &diag->root_inode_lba);
    }
    diag->descriptor_mapping_valid = journal_mapping && root_mapping;
    diag->pfs_super_valid =
        diag->apa_header_magic_valid && diag->apa_header_checksum_valid &&
        diag->apa_sub_mapping_valid && super->magic == PFS_RAW_SUPER_MAGIC &&
        super->version <= PFS_RAW_FORMAT_VERSION && zone_valid &&
        diag->subpartition_class == PFS_SUBPART_MATCH &&
        diag->descriptor_mapping_valid;

    if (journal_mapping) {
        diag->journal_read_result =
            read_sectors(diag->journal_lba, 2, component);
        if (diag->journal_read_result >= 0) {
            journal = (const pfs_raw_journal_t *)component;
            hash_buffer(component, sizeof(component), diag->journal_sha256);
            diag->journal_magic = journal->magic;
            diag->journal_num = journal->num;
            diag->journal_checksum_stored = journal->checksum;
            calculated = checksum_words(component, 2) & 0xffffu;
            diag->journal_checksum_calculated = calculated;
            diag->journal_checksum_valid =
                calculated == journal->checksum;
            if (journal->magic != PFS_RAW_JOURNAL_MAGIC ||
                !diag->journal_checksum_valid)
                diag->journal_class = PFS_JOURNAL_INVALID;
            else if (journal->num == 0)
                diag->journal_class = PFS_JOURNAL_CLEAN;
            else
                diag->journal_class = PFS_JOURNAL_RECOVERY_REQUIRED;
        }
    }

    if (root_mapping) {
        diag->root_inode_read_result =
            read_sectors(diag->root_inode_lba, 2, component);
        if (diag->root_inode_read_result >= 0) {
            u32 directory_lba = 0;
            inode = (const pfs_raw_inode_t *)component;
            hash_buffer(component, sizeof(component),
                        diag->root_inode_sha256);
            diag->root_inode_magic = inode->magic;
            diag->root_inode_checksum_stored = inode->checksum;
            calculated = checksum_words(component, 1);
            diag->root_inode_checksum_calculated = calculated;
            diag->root_inode_checksum_valid = calculated == inode->checksum;
            diag->root_inode_mode = inode->mode;
            diag->root_inode_attr = inode->attr;
            diag->root_inode_size = inode->size;
            diag->root_inode_number_blocks = inode->number_blocks;
            diag->root_inode_number_data = inode->number_data;
            diag->root_inode_number_segdesg = inode->number_segdesg;
            diag->root_inode_subpart = inode->subpart;
            diag->root_inode_location_valid =
                inode->inode_block.number == super->root.number &&
                inode->inode_block.subpart == super->root.subpart;
            diag->root_inode_type_valid = FIO_S_ISDIR(inode->mode);
            diag->root_inode_extents_valid =
                inode->number_data >= 2 && inode->data[1].count > 0 &&
                map_blockinfo(header, entry, &inode->data[1],
                              diag->zone_scale, 2, disk_sectors,
                              &directory_lba);
            if (diag->root_inode_extents_valid) {
                diag->root_directory_lba = directory_lba;
                diag->root_directory_read_result =
                    read_sectors(directory_lba, 2, component);
                if (diag->root_directory_read_result >= 0) {
                    hash_buffer(component, sizeof(component),
                                diag->root_directory_sha256);
                    diag->root_directory_valid =
                        validate_root_directory(component, diag);
                }
            }
        }
    }

    if (zone_valid) {
        u64 relative = PFS_RAW_SUPER_SECTOR + (1u << diag->zone_scale);
        if (relative + 2u <= entry->size &&
            (u64)entry->start + relative + 2u <= disk_sectors) {
            diag->bitmap_lba = entry->start + (u32)relative;
            diag->bitmap_read_result =
                read_sectors(diag->bitmap_lba, 2, component);
            if (diag->bitmap_read_result >= 0) {
                diag->bitmap_first_word = ((const u32 *)component)[0];
                diag->bitmap_reserved_bit0_set =
                    (diag->bitmap_first_word & 1u) != 0;
                diag->bitmap_structure_valid =
                    diag->bitmap_size_sectors > 0 &&
                    diag->bitmap_reserved_bit0_set;
                hash_buffer(component, sizeof(component),
                            diag->bitmap_sha256);
            }
        }
    }

    diag->pfs_root_valid =
        diag->root_inode_read_result >= 0 &&
        diag->root_inode_checksum_valid &&
        diag->root_inode_location_valid && diag->root_inode_type_valid &&
        diag->root_inode_extents_valid && diag->root_directory_valid;
    diag->raw_pfs_read_valid =
        diag->apa_header_read_result >= 0 &&
        diag->primary_read_result >= 0 && diag->backup_read_result >= 0 &&
        diag->journal_read_result >= 0 &&
        diag->root_inode_read_result >= 0 &&
        diag->root_directory_read_result >= 0 &&
        diag->bitmap_read_result >= 0;
    diag->pfs_structure_valid =
        diag->raw_pfs_read_valid && diag->pfs_super_valid &&
        diag->pfs_root_valid && diag->bitmap_structure_valid &&
        (diag->journal_class == PFS_JOURNAL_CLEAN ||
         diag->journal_class == PFS_JOURNAL_RECOVERY_REQUIRED);
}

static void scan_pfs(inspector_data_t *data)
{
    unsigned int index;

    data->raw_pfs_target_count = 4;
    for (index = 0; index < data->apa_count; ++index) {
        apa_entry_t *entry = &data->apa[index];

        if (!is_system_pfs_name(entry->name)) {
            entry->raw_pfs.applicable = 0;
            entry->raw_pfs.subpartition_class =
                PFS_SUBPART_NOT_APPLICABLE;
            continue;
        }
        data->raw_pfs_found_count++;
        inspect_raw_pfs(data, entry);
        if (entry->raw_pfs.pfs_structure_valid)
            data->raw_pfs_valid_count++;

        /* The destructive test validates the on-disk result only via RAW reads. */
    }
    data->pfs_scan_completed =
        data->hdd_final_dread_result == 0 && !data->apa_overflow;
    data->raw_pfs_discovery_valid =
        data->pfs_scan_completed && data->raw_pfs_found_count == 4 &&
        data->raw_pfs_valid_count == 4;
    data->pfs_discovery_valid = data->raw_pfs_discovery_valid;
}

void inspector_initialize(inspector_data_t *data)
{
    unsigned int index;

    memset(data, 0, sizeof(*data));
    data->hdd_status = data->hdd_formatver = data->hdd_totalsector = -1;
    data->hdd_maxsector = data->hdd_getmaxlba48 = data->hdd_islba48 = -1;
    data->mbr_read_result = -1;
    SifInitRpc(0);
    data->lmb_patch_result = sbv_patch_enable_lmb();
    data->prefix_patch_result = sbv_patch_disable_prefix_check();
    for (index = 0; index < INSPECTOR_MODULE_COUNT; ++index)
        load_module(data, index);
    data->filexio_init_result = fileXioInit();
    data->cdvd_init_result = sceCdInit(SCECdINoD);
    data->cdvd_disk_type = sceCdGetDiskType();
    data->cdvd_status = sceCdStatus();
    read_romver(data);
}

void inspector_scan_layout(inspector_data_t *data)
{
    data->hdd_status =
        fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0);
    data->hdd_formatver =
        fileXioDevctl("hdd0:", HDIOC_FORMATVER, NULL, 0, NULL, 0);
    data->hdd_totalsector =
        fileXioDevctl("hdd0:", HDIOC_TOTALSECTOR, NULL, 0, NULL, 0);
    data->hdd_maxsector =
        fileXioDevctl("hdd0:", HDIOC_MAXSECTOR, NULL, 0, NULL, 0);
    data->hdd_getmaxlba48 =
        fileXioDevctl("hdd0:", HDIOC_GETMAXLBA48, NULL, 0, NULL, 0);
    data->hdd_islba48 =
        fileXioDevctl("hdd0:", HDIOC_ISLBA48, NULL, 0, NULL, 0);
    enumerate_hdd(data);
    parse_mbr(data);
}

void inspector_scan(inspector_data_t *data)
{
    inspector_scan_layout(data);
    data->optional_hashes[0] = (optional_hash_t){"sector_0x2000", 0x2000, 0x2000, -1, ""};
    data->optional_hashes[1] = (optional_hash_t){"range_0x2020_0x23C6", 0x2020, 0x23C6, -1, ""};
    data->optional_hashes[2] = (optional_hash_t){"range_0x2440_0x27E6", 0x2440, 0x27E6, -1, ""};
    hash_sector_range(&data->optional_hashes[0]);
    hash_sector_range(&data->optional_hashes[1]);
    hash_sector_range(&data->optional_hashes[2]);
    scan_pfs(data);
}
