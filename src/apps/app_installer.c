#include <ctype.h>
#include <errno.h>
#include <hdd-ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "apps/app_installer.h"
#include "kelf.h"
#include "sha256.h"
#include "source_media.h"

#define APA_MAGIC 0x00415041u
#define APA_HEADER_BYTES 1024u
#define SECTOR_BYTES 512u
#define APP_PARTITION_PREFIX "PP.RBX."
#define APP_PARTITION_HASH_LENGTH 16u
#define APP_FIXED_PARTITION_PREFIX "PP.APPS-"
#define XMB_PARTITION_PREFIX "PP."
#define APP_PARTITION_SECTORS 0x00040000u
#define APP_DISCOVERY_MAX 128u
#define APP_PFS_ZONE_SIZE 8192
#define COPY_BUFFER_SIZE (64u * 1024u)
#define INI_CAPACITY 2048u
#define MAX_APP_ELF_BYTES \
    (KELF_LENGTH - HDLOADER_KELF_HEADER_LEN - HDLOADER_KELF_FOOTER_LEN)
#define MAX_COVER_BYTES (1024u * 1024u)
#define MIN_COVER_DIMENSION 16u
#define MAX_COVER_DIMENSION 256u

extern unsigned char default_cover_png[] __attribute__((aligned(16)));
extern unsigned int size_default_cover_png;

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
               "APA header layout must be 1024 bytes");

static unsigned char copy_buffer[COPY_BUFFER_SIZE]
    __attribute__((aligned(64)));
static unsigned char apa_buffer[APA_HEADER_BYTES]
    __attribute__((aligned(64)));
static unsigned char readback_sector[SECTOR_BYTES]
    __attribute__((aligned(64)));
static unsigned char write_transfer_buffer[sizeof(hddAtaTransfer_t) +
                                            SECTOR_BYTES]
    __attribute__((aligned(64)));

static void set_preflight_failure(app_preflight_t *item, int result,
                                  const char *name)
{
    if (item->failure_result != 0)
        return;
    item->failure_result = result < 0 ? result : -EIO;
    snprintf(item->failure_item, sizeof(item->failure_item), "%s", name);
}

static void set_install_failure(app_install_result_t *result,
                                unsigned int step, int value,
                                const char *operation, const char *item)
{
    if (result->failure_result != 0)
        return;
    result->failed_step = (int)step;
    result->failure_result = value < 0 ? value : -EIO;
    snprintf(result->failure_operation, sizeof(result->failure_operation),
             "%s", operation);
    snprintf(result->failure_item, sizeof(result->failure_item), "%s", item);
}

static int text_equal_ci(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    if (text_length < suffix_length)
        return 0;
    return text_equal_ci(text + text_length - suffix_length, suffix);
}

static int starts_with_ci(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text == '\0' ||
            tolower((unsigned char)*text) !=
                tolower((unsigned char)*prefix))
            return 0;
        ++text;
        ++prefix;
    }
    return 1;
}

static int safe_component(const char *text, size_t maximum)
{
    size_t index;
    size_t length = strlen(text);

    if (length == 0 || length > maximum || text[0] == '.')
        return 0;
    for (index = 0; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];

        if (value < 0x20 || value > 0x7e || value == '/' ||
            value == '\\' || value == ':' || value == '=')
            return 0;
    }
    return 1;
}

static int normalize_id(const char *source, char output[48])
{
    size_t input;
    size_t used = 0;
    int separator = 0;

    for (input = 0; source[input] != '\0'; ++input) {
        unsigned char value = (unsigned char)source[input];

        if (isalnum(value)) {
            if (used + 1 >= 48)
                return -ENAMETOOLONG;
            output[used++] = (char)tolower(value);
            separator = 0;
        } else if (value == '-' || value == '_' || value == ' ') {
            if (used != 0 && !separator) {
                if (used + 1 >= 48)
                    return -ENAMETOOLONG;
                output[used++] = '-';
                separator = 1;
            }
        } else {
            return -EINVAL;
        }
    }
    while (used != 0 && output[used - 1] == '-')
        --used;
    output[used] = '\0';
    return used == 0 ? -EINVAL : 0;
}

static void hash_memory(const void *data, size_t size, char hex[65])
{
    sha256_context_t context;
    unsigned char digest[SHA256_DIGEST_SIZE];

    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, digest);
    sha256_to_hex(digest, hex);
}

static int hash_file(const char *path, u64 *size, char hex[65],
                     u64 *work_done, u64 total_work,
                     app_progress_callback_t progress, void *progress_context)
{
    sha256_context_t hash;
    unsigned char digest[SHA256_DIGEST_SIZE];
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);

    if (fd < 0)
        return fd;
    *size = 0;
    sha256_init(&hash);
    for (;;) {
        int read_result = fileXioRead(
            fd, copy_buffer, source_media_read_size(sizeof(copy_buffer)));

        if (read_result < 0) {
            fileXioClose(fd);
            return read_result;
        }
        if (read_result == 0)
            break;
        sha256_update(&hash, copy_buffer, (size_t)read_result);
        *size += (u64)read_result;
        if (work_done != NULL && progress != NULL) {
            *work_done += (u64)read_result;
            progress(3, APP_STEP_COUNT, "COPY AND VERIFY APPLICATION",
                     total_work == 0 ? 0u :
                     (unsigned int)(*work_done * 100u / total_work),
                     progress_context);
        }
    }
    {
        int close_result = fileXioClose(fd);

        if (close_result < 0)
            return close_result;
    }
    sha256_final(&hash, digest);
    sha256_to_hex(digest, hex);
    return 0;
}

static int hash_equal(const char *left, const char *right)
{
    return text_equal_ci(left, right);
}

static u32 read_be32(const unsigned char *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | data[3];
}

static u32 read_le32(const unsigned char *data)
{
    return (u32)data[0] | ((u32)data[1] << 8) |
           ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static int validate_png(const char *path, u64 *size, char hash[65])
{
    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    unsigned char header[24];
    u32 width;
    u32 height;
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    int result;

    if (fd < 0)
        return fd;
    result = fileXioRead(fd, header, sizeof(header));
    fileXioClose(fd);
    if (result != (int)sizeof(header) ||
        memcmp(header, signature, sizeof(signature)) != 0 ||
        memcmp(header + 12, "IHDR", 4) != 0)
        return -EINVAL;
    width = read_be32(header + 16);
    height = read_be32(header + 20);
    if (width < MIN_COVER_DIMENSION || width > MAX_COVER_DIMENSION ||
        height < MIN_COVER_DIMENSION || height > MAX_COVER_DIMENSION)
        return -EINVAL;
    result = hash_file(path, size, hash, NULL, 0, NULL, NULL);
    if (result < 0)
        return result;
    return *size <= MAX_COVER_BYTES ? 0 : -EFBIG;
}

static int validate_elf(const char *path, u64 *size, char hash[65])
{
    unsigned char header[20];
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    int result;

    if (fd < 0)
        return fd;
    result = fileXioRead(fd, header, sizeof(header));
    fileXioClose(fd);
    if (result != (int)sizeof(header) || header[0] != 0x7f ||
        memcmp(header + 1, "ELF", 3) != 0 || header[4] != 1 ||
        header[5] != 1 || header[6] != 1 || header[18] != 8 ||
        header[19] != 0)
        return -ENOEXEC;
    result = hash_file(path, size, hash, NULL, 0, NULL, NULL);
    if (result < 0)
        return result;
    return *size <= MAX_APP_ELF_BYTES ? 0 : -EFBIG;
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

static int system_cnf_boots_pfs_application(char *config)
{
    char *save = NULL;
    char *line;

    for (line = strtok_r(config, "\r\n", &save); line != NULL;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *key = trim(line);
        char *separator = strchr(key, '=');
        char *value;

        if (separator == NULL)
            continue;
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);
        if (!text_equal_ci(key, "BOOT2"))
            continue;
        return starts_with_ci(value, "pfs:/") &&
               (ends_with_ci(value, ".KELF") ||
                ends_with_ci(value, ".ELF"));
    }
    return 0;
}

static int copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);

    if (length >= capacity)
        return -ENAMETOOLONG;
    memcpy(destination, source, length + 1u);
    return 0;
}

static int read_config(app_package_t *package)
{
    char path[256];
    char buffer[INI_CAPACITY + 1u];
    char *save = NULL;
    char *line;
    int fd;
    int read_result;

    snprintf(path, sizeof(path), "%s/app.ini", package->source_root);
    fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    if (fd < 0)
        return fd == -ENOENT ? 0 : fd;
    package->config_present = 1;
    read_result = fileXioRead(fd, buffer, INI_CAPACITY);
    fileXioClose(fd);
    if (read_result < 0)
        return read_result;
    if (read_result == (int)INI_CAPACITY)
        return -EFBIG;
    buffer[read_result] = '\0';
    for (line = strtok_r(buffer, "\r\n", &save); line != NULL;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *key = trim(line);
        char *separator;
        char *value;

        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        separator = strchr(key, '=');
        if (separator == NULL)
            return -EINVAL;
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);
        if (*value == '\0')
            return -EINVAL;
        if (text_equal_ci(key, "id")) {
            int result = normalize_id(value, package->id);
            if (result < 0)
                return result;
        } else if (text_equal_ci(key, "title")) {
            if (!safe_component(value, sizeof(package->title) - 1u))
                return -EINVAL;
            if (copy_text(package->title, sizeof(package->title), value) < 0)
                return -ENAMETOOLONG;
        } else if (text_equal_ci(key, "subtitle")) {
            if (!safe_component(value, sizeof(package->subtitle) - 1u))
                return -EINVAL;
            if (copy_text(package->subtitle, sizeof(package->subtitle), value) < 0)
                return -ENAMETOOLONG;
        } else if (text_equal_ci(key, "elf")) {
            if (!safe_component(value, sizeof(package->elf_name) - 1u) ||
                !ends_with_ci(value, ".elf"))
                return -EINVAL;
            if (copy_text(package->elf_name, sizeof(package->elf_name), value) < 0)
                return -ENAMETOOLONG;
        } else {
            return -EINVAL;
        }
    }
    return 0;
}

static void make_partition_name(app_package_t *package)
{
    sha256_context_t context;
    unsigned char digest[SHA256_DIGEST_SIZE];
    char hex[65];

    sha256_init(&context);
    sha256_update(&context, package->id, strlen(package->id));
    sha256_final(&context, digest);
    sha256_to_hex(digest, hex);
    snprintf(package->partition_name, sizeof(package->partition_name),
             "PP.RBX.%.16s", hex);
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

static int write_sector(u32 lba, const void *data)
{
    hddAtaTransfer_t *transfer =
        (hddAtaTransfer_t *)write_transfer_buffer;

    transfer->lba = lba;
    transfer->size = 1;
    memcpy(transfer->data, data, SECTOR_BYTES);
    return fileXioDevctl("hdd0:", HDIOC_WRITESECTOR, transfer,
                         sizeof(*transfer) + SECTOR_BYTES, NULL, 0);
}

static int apa_checksum_valid(const apa_header_public_t *header)
{
    const u32 *words = (const u32 *)header;
    u32 checksum = 0;
    unsigned int index;

    for (index = 1; index < APA_HEADER_BYTES / sizeof(u32); ++index)
        checksum += words[index];
    return header->magic == APA_MAGIC && header->checksum == checksum;
}

static int managed_partition_name_valid(const char *name)
{
    size_t prefix_length = strlen(APP_PARTITION_PREFIX);
    size_t index;

    if (strncmp(name, APP_PARTITION_PREFIX, prefix_length) != 0 ||
        strlen(name) != prefix_length + APP_PARTITION_HASH_LENGTH)
        return 0;
    for (index = prefix_length; name[index] != '\0'; ++index) {
        if (!isdigit((unsigned char)name[index]) &&
            (name[index] < 'a' || name[index] > 'f'))
            return 0;
    }
    return 1;
}

static int xmb_partition_name_candidate(const char *name)
{
    size_t length = strlen(name);

    return length > strlen(XMB_PARTITION_PREFIX) && length < 32u &&
           strncmp(name, XMB_PARTITION_PREFIX,
                   strlen(XMB_PARTITION_PREFIX)) == 0;
}

static int installer_owned_partition_name(const char *name)
{
    return managed_partition_name_valid(name) ||
           strncmp(name, APP_FIXED_PARTITION_PREFIX,
                   strlen(APP_FIXED_PARTITION_PREFIX)) == 0;
}

static int validate_partition_header(const char *name, u32 start,
                                     u32 expected_length)
{
    const apa_header_public_t *header =
        (const apa_header_public_t *)apa_buffer;
    char expected_id[32];
    int result = read_sectors(start, 2, apa_buffer);

    if (result < 0)
        return result;
    memset(expected_id, 0, sizeof(expected_id));
    snprintf(expected_id, sizeof(expected_id), "%s", name);
    if (!apa_checksum_valid(header) ||
        memcmp(header->id, expected_id, sizeof(header->id)) != 0 ||
        header->start != start || header->length != expected_length ||
        header->type != 0x0100u || header->nsub != 0)
        return -EINVAL;
    return 0;
}

static int validate_target_header(const app_package_t *package,
                                  u32 start, u32 expected_length)
{
    return validate_partition_header(package->partition_name, start,
                                     expected_length);
}

static int validate_xmb_application_registration(u32 start)
{
    char system_cnf[SECTOR_BYTES + 1u];
    u32 system_offset;
    u32 system_size;
    u32 icon_offset;
    u32 icon_size;
    int result;

    result = read_sectors(start + 8u, 1, readback_sector);
    if (result < 0)
        return result;
    if (memcmp(readback_sector, "PS2ICON3D", 9) != 0)
        return -EINVAL;
    system_offset = read_le32(readback_sector + 0x10);
    system_size = read_le32(readback_sector + 0x14);
    icon_offset = read_le32(readback_sector + 0x18);
    icon_size = read_le32(readback_sector + 0x1c);
    if (system_offset != 0x0200u || icon_offset != 0x0400u ||
        system_size == 0 || system_size > SECTOR_BYTES ||
        icon_size == 0 || icon_size > SECTOR_BYTES)
        return -EINVAL;

    result = read_sectors(start + 9u, 1, readback_sector);
    if (result < 0)
        return result;
    memcpy(system_cnf, readback_sector, system_size);
    system_cnf[system_size] = '\0';
    if (!system_cnf_boots_pfs_application(system_cnf))
        return -EINVAL;

    result = read_sectors(start + 10u, 1, readback_sector);
    if (result < 0)
        return result;
    if (memcmp(readback_sector, "PS2X", 4) != 0)
        return -EINVAL;
    return 0;
}

static void set_uninstall_failure(app_uninstall_result_t *result,
                                  int value, const char *partition)
{
    if (result->failure_result != 0)
        return;
    result->failure_result = value < 0 ? value : -EIO;
    snprintf(result->failure_partition,
             sizeof(result->failure_partition), "%.31s",
             partition != NULL ? partition : "");
}

int app_scan_managed_partitions(app_uninstall_result_t *result)
{
    static app_managed_partition_t discovered[APP_DISCOVERY_MAX];
    iox_dirent_t entry;
    unsigned int discovered_count = 0;
    unsigned int index;
    int fd;
    int read_result;

    memset(result, 0, sizeof(*result));
    fd = fileXioDopen("hdd0:");
    result->scan_result = fd;
    if (fd < 0) {
        set_uninstall_failure(result, fd, "hdd0:");
        return fd;
    }
    read_result = 0;
    for (;;) {
        app_managed_partition_t *candidate;

        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        if (!xmb_partition_name_candidate(entry.name) ||
            entry.stat.size != APP_PARTITION_SECTORS)
            continue;
        if (discovered_count >= APP_DISCOVERY_MAX) {
            read_result = -E2BIG;
            break;
        }
        for (index = 0; index < discovered_count; ++index) {
            if (strcmp(discovered[index].name, entry.name) == 0) {
                read_result = -EEXIST;
                break;
            }
        }
        if (read_result < 0)
            break;
        candidate = &discovered[discovered_count++];
        memset(candidate, 0, sizeof(*candidate));
        snprintf(candidate->name, sizeof(candidate->name), "%s",
                 entry.name);
        candidate->start = entry.stat.private_5;
        candidate->length = entry.stat.size;
    }
    {
        int close_result = fileXioDclose(fd);

        if (read_result >= 0 && close_result < 0)
            read_result = close_result;
    }
    result->scan_result = read_result < 0 ? read_result : 0;
    if (result->scan_result < 0) {
        set_uninstall_failure(result, result->scan_result, "hdd0:");
        return result->scan_result;
    }
    for (index = 0; index < discovered_count; ++index) {
        app_managed_partition_t *candidate = &discovered[index];
        int value;

        value = validate_partition_header(candidate->name,
                                          candidate->start,
                                          candidate->length);
        if (value < 0) {
            if (value != -EINVAL ||
                installer_owned_partition_name(candidate->name)) {
                result->validation_result = value;
                set_uninstall_failure(result, value, candidate->name);
                return value;
            }
            continue;
        }
        value = validate_xmb_application_registration(candidate->start);
        if (value < 0) {
            if (value != -EINVAL ||
                installer_owned_partition_name(candidate->name)) {
                result->validation_result = value;
                set_uninstall_failure(result, value, candidate->name);
                return value;
            }
            continue;
        }
        if (result->candidate_count >= APP_MANAGED_PARTITION_MAX) {
            result->validation_result = -E2BIG;
            set_uninstall_failure(result, -E2BIG, candidate->name);
            return -E2BIG;
        }
        result->partitions[result->candidate_count++] = *candidate;
        ++result->validated_count;
    }
    result->validation_result = 0;
    return 0;
}

void app_uninstall_managed_partitions(app_uninstall_result_t *result)
{
    static app_uninstall_result_t verification;
    unsigned int index;
    int value;

    value = app_scan_managed_partitions(result);
    result->requested = 1;
    if (value < 0)
        return;
    if (result->candidate_count == 0) {
        result->success = 1;
        return;
    }

    fileXioUmount("pfs0:");
    for (index = 0; index < result->candidate_count; ++index) {
        char target[48];

        snprintf(target, sizeof(target), "hdd0:%s",
                 result->partitions[index].name);
        value = fileXioRemove(target);
        if (value < 0) {
            /* Best effort: persist any earlier removals before reporting a
               partial failure.  The failing partition remains identified in
               the diagnostic result. */
            fileXioDevctl("hdd0:", HDIOC_FLUSH, NULL, 0, NULL, 0);
            result->removal_result = value;
            set_uninstall_failure(result, value,
                                  result->partitions[index].name);
            return;
        }
        ++result->removed_count;
    }
    value = fileXioDevctl("hdd0:", HDIOC_FLUSH, NULL, 0, NULL, 0);
    result->removal_result = value < 0 ? value : 0;
    if (value < 0) {
        set_uninstall_failure(result, value, "hdd0:");
        return;
    }

    value = app_scan_managed_partitions(&verification);
    result->verification_result = value;
    if (value < 0) {
        set_uninstall_failure(result, value,
                              verification.failure_partition);
        return;
    }
    if (verification.candidate_count != 0) {
        result->verification_result = -EIO;
        set_uninstall_failure(result, -EIO,
                              verification.partitions[0].name);
        return;
    }
    result->success = 1;
}

static int scan_hdd_layout(app_preflight_t *item)
{
    static const char *const required[] = {
        "__mbr", "__net", "__system", "__sysconf", "__common"};
    unsigned int found_mask = 0;
    iox_dirent_t entry;
    int fd = fileXioDopen("hdd0:");
    int read_result = fd;

    if (fd < 0)
        return fd;
    for (;;) {
        unsigned int index;

        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        for (index = 0; index < sizeof(required) / sizeof(required[0]);
             ++index) {
            if (strcmp(entry.name, required[index]) == 0)
                found_mask |= 1u << index;
        }
        if (strcmp(entry.name, item->package.partition_name) == 0) {
            if (item->target_exists) {
                read_result = -EINVAL;
                break;
            }
            item->target_exists = 1;
            item->target_start = entry.stat.private_5;
            item->target_length = entry.stat.size;
        }
    }
    {
        int close_result = fileXioDclose(fd);

        if (read_result >= 0 && close_result < 0)
            read_result = close_result;
    }
    if (read_result < 0)
        return read_result;
    item->required_partitions_valid =
        found_mask == (1u << (sizeof(required) / sizeof(required[0]))) - 1u;
    if (!item->required_partitions_valid)
        return -EINVAL;
    if (item->target_exists) {
        if (item->target_length != APP_PARTITION_SECTORS)
            return -EINVAL;
        return validate_target_header(&item->package, item->target_start,
                                      item->target_length);
    }
    return 0;
}

static int find_package_files(app_preflight_t *item)
{
    iox_dirent_t entry;
    int fd = fileXioDopen(item->package.source_root);
    int result = fd;
    unsigned int elf_count = 0;
    char discovered_elf[64] = "";
    char source_root[192];

    if (copy_text(source_root, sizeof(source_root),
                  item->package.source_root) < 0)
        return -ENAMETOOLONG;

    if (fd < 0)
        return fd;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        result = fileXioDread(fd, &entry);
        if (result <= 0)
            break;
        if (FIO_S_ISDIR(entry.stat.mode))
            continue;
        if (ends_with_ci(entry.name, ".elf")) {
            ++elf_count;
            if (elf_count == 1 &&
                copy_text(discovered_elf, sizeof(discovered_elf),
                          entry.name) < 0) {
                result = -ENAMETOOLONG;
                break;
            }
        }
        if (text_equal_ci(entry.name, "cover.png")) {
            item->package.cover_present = 1;
            snprintf(item->package.source_cover,
                     sizeof(item->package.source_cover), "%s/cover.png",
                     source_root);
        }
    }
    if (source_media_dread_is_eof(result))
        result = 0;
    {
        int close_result = fileXioDclose(fd);

        if (result >= 0 && close_result < 0)
            result = close_result;
    }
    if (result < 0)
        return result;
    item->config_result = read_config(&item->package);
    if (item->config_result < 0)
        return item->config_result;
    if (item->package.elf_name[0] == '\0') {
        if (elf_count != 1)
            return elf_count == 0 ? -ENOENT : -EEXIST;
        if (copy_text(item->package.elf_name,
                      sizeof(item->package.elf_name), discovered_elf) < 0)
            return -ENAMETOOLONG;
    }
    snprintf(item->package.source_elf, sizeof(item->package.source_elf),
             "%s/%s", source_root, item->package.elf_name);
    return 0;
}

static void scan_package(const char *folder, app_preflight_t *item)
{
    int result;

    memset(item, 0, sizeof(*item));
    if (!safe_component(folder, 31u) ||
        copy_text(item->package.folder, sizeof(item->package.folder),
                  folder) < 0 ||
        copy_text(item->package.title, sizeof(item->package.title),
                  folder) < 0) {
        set_preflight_failure(item, -ENAMETOOLONG, "folder name");
        return;
    }
    snprintf(item->package.subtitle, sizeof(item->package.subtitle),
             "Homebrew Application");
    snprintf(item->package.source_root, sizeof(item->package.source_root),
             "%s/%s", APP_USB_ROOT, folder);
    result = normalize_id(folder, item->package.id);
    if (result < 0) {
        set_preflight_failure(item, result, "folder name / id");
        return;
    }
    result = find_package_files(item);
    if (result < 0) {
        set_preflight_failure(item, result,
                              item->config_result < 0 ? "app.ini" :
                              "ELF selection");
        return;
    }
    make_partition_name(&item->package);
    item->elf_result = validate_elf(item->package.source_elf,
                                    &item->elf_size, item->elf_sha256);
    if (item->elf_result < 0) {
        set_preflight_failure(item, item->elf_result, item->package.elf_name);
        return;
    }
    if (item->package.cover_present) {
        item->cover_result = validate_png(item->package.source_cover,
                                          &item->cover_size,
                                          item->cover_sha256);
        if (item->cover_result < 0) {
            set_preflight_failure(item, item->cover_result, "cover.png");
            return;
        }
    } else {
        item->cover_size = size_default_cover_png;
        hash_memory(default_cover_png, size_default_cover_png,
                    item->cover_sha256);
    }
    item->hdd_status =
        fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0);
    item->format_version =
        fileXioDevctl("hdd0:", HDIOC_FORMATVER, NULL, 0, NULL, 0);
    if (item->hdd_status < 0 || item->format_version < 0) {
        set_preflight_failure(item,
            item->hdd_status < 0 ? item->hdd_status : item->format_version,
            "hdd0 status / format version");
        return;
    }
    result = scan_hdd_layout(item);
    if (result < 0) {
        set_preflight_failure(item, result, "hdd0 layout / target");
        return;
    }
    item->valid = 1;
}

void app_scan_catalog(app_catalog_t *catalog)
{
    iox_dirent_t entry;
    int fd;
    int read_result;
    unsigned int index;

    memset(catalog, 0, sizeof(*catalog));
    fd = fileXioDopen(APP_USB_ROOT);
    catalog->root_open_result = fd;
    if (fd < 0)
        return;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        if (!FIO_S_ISDIR(entry.stat.mode) || entry.name[0] == '.' ||
            !safe_component(entry.name, 31u))
            continue;
        if (catalog->count >= APP_MAX_COUNT) {
            catalog->truncated = 1;
            continue;
        }
        scan_package(entry.name, &catalog->apps[catalog->count]);
        ++catalog->count;
    }
    if (source_media_dread_is_eof(read_result))
        read_result = 0;
    catalog->root_close_result = fileXioDclose(fd);
    if (read_result < 0)
        catalog->root_close_result = read_result;

    for (index = 0; index < catalog->count; ++index) {
        unsigned int other;
        app_preflight_t *item = &catalog->apps[index];

        if (!item->valid)
            continue;
        for (other = 0; other < index; ++other) {
            if (catalog->apps[other].valid &&
                strcmp(item->package.id,
                       catalog->apps[other].package.id) == 0) {
                item->valid = 0;
                set_preflight_failure(item, -EEXIST, "duplicate app id");
                break;
            }
        }
        if (!item->valid)
            continue;
        ++catalog->valid_count;
        if (item->target_exists)
            ++catalog->update_count;
        else
            ++catalog->new_count;
    }
}

static int refresh_target(const app_package_t *package,
                          app_install_result_t *result)
{
    iox_dirent_t entry;
    int fd = fileXioDopen("hdd0:");
    int read_result = fd;
    int found = 0;

    if (fd < 0)
        return fd;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        if (strcmp(entry.name, package->partition_name) == 0) {
            result->target_start = entry.stat.private_5;
            result->target_length = entry.stat.size;
            found = 1;
        }
    }
    {
        int close_result = fileXioDclose(fd);
        if (read_result >= 0 && close_result < 0)
            read_result = close_result;
    }
    if (read_result < 0)
        return read_result;
    if (!found || result->target_length != APP_PARTITION_SECTORS)
        return -EINVAL;
    return validate_target_header(package, result->target_start,
                                  result->target_length);
}

static int create_target_partition(const app_package_t *package)
{
    char target[96];
    int fd;

    snprintf(target, sizeof(target), "hdd0:%s,,,128M,PFS",
             package->partition_name);
    fd = fileXioOpen(target, FIO_O_RDWR | FIO_O_CREAT, 0);
    if (fd < 0)
        return fd;
    return fileXioClose(fd);
}

static int ensure_directory(const char *path)
{
    int result = fileXioMkdir(path, 0777);

    if (result >= 0)
        return 0;
    {
        int fd = fileXioDopen(path);
        if (fd < 0)
            return result;
        return fileXioDclose(fd);
    }
}

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    size_t offset = 0;

    while (offset < size) {
        int result = fileXioWrite(fd, bytes + offset, size - offset);

        if (result <= 0)
            return result < 0 ? result : -EIO;
        offset += (size_t)result;
    }
    return 0;
}

static int write_memory_verified(const char *path, const void *data,
                                 size_t size, u64 *copied_bytes)
{
    char expected[65];
    char actual[65];
    u64 actual_size = 0;
    int fd;
    int result;

    hash_memory(data, size, expected);
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    result = write_all(fd, data, size);
    {
        int close_result = fileXioClose(fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
    }
    if (result < 0)
        return result;
    *copied_bytes += size;
    result = hash_file(path, &actual_size, actual, NULL, 0, NULL, NULL);
    if (result < 0)
        return result;
    return actual_size == size && hash_equal(expected, actual) ? 0 : -EIO;
}

static int copy_file_verified(const char *source, const char *destination,
                              u64 expected_size, const char *expected_hash,
                              u64 *copied_bytes, u64 *work_done,
                              u64 total_work,
                              app_progress_callback_t progress,
                              void *progress_context,
                              char actual_hash[65])
{
    int source_fd = fileXioOpen(source, FIO_O_RDONLY, 0);
    int destination_fd;
    int result = 0;
    u64 actual_size = 0;

    if (source_fd < 0)
        return source_fd;
    destination_fd = fileXioOpen(
        destination, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (destination_fd < 0) {
        fileXioClose(source_fd);
        return destination_fd;
    }
    for (;;) {
        int read_result = fileXioRead(
            source_fd, copy_buffer,
            source_media_read_size(sizeof(copy_buffer)));

        if (read_result < 0) {
            result = read_result;
            break;
        }
        if (read_result == 0)
            break;
        result = write_all(destination_fd, copy_buffer, (size_t)read_result);
        if (result < 0)
            break;
        *copied_bytes += (u64)read_result;
        *work_done += (u64)read_result;
        progress(3, APP_STEP_COUNT, "COPY AND VERIFY APPLICATION",
                 (unsigned int)(*work_done * 100u / total_work),
                 progress_context);
    }
    {
        int close_result = fileXioClose(source_fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
        close_result = fileXioClose(destination_fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
    }
    if (result < 0)
        return result;
    result = hash_file(destination, &actual_size, actual_hash, work_done,
                       total_work, progress, progress_context);
    if (result < 0)
        return result;
    return actual_size == expected_size &&
           hash_equal(actual_hash, expected_hash) ? 0 : -EIO;
}

static void report_copy_progress(u64 *work_done, u64 amount,
                                 u64 total_work,
                                 app_progress_callback_t progress,
                                 void *progress_context)
{
    *work_done += amount;
    progress(3, APP_STEP_COUNT, "COPY AND VERIFY APPLICATION",
             (unsigned int)(*work_done * 100u / total_work),
             progress_context);
}

static int read_exact(int fd, void *data, size_t size)
{
    unsigned char *bytes = data;
    size_t offset = 0;

    while (offset < size) {
        int result = fileXioRead(
            fd, bytes + offset, source_media_read_size(size - offset));

        if (result <= 0)
            return result < 0 ? result : -EIO;
        offset += (size_t)result;
    }
    return 0;
}

static int write_direct_kelf_verified(const char *source,
                                      const char *destination,
                                      u64 source_size,
                                      const char *expected_hash,
                                      u64 *copied_bytes,
                                      u64 *work_done, u64 total_work,
                                      app_progress_callback_t progress,
                                      void *progress_context,
                                      char actual_hash[65])
{
    sha256_context_t hash;
    unsigned char digest[SHA256_DIGEST_SIZE];
    u64 remaining;
    u64 padding_size;
    int source_fd;
    int destination_fd;
    int result = 0;

    if (source_size > MAX_APP_ELF_BYTES)
        return -EFBIG;
    source_fd = fileXioOpen(source, FIO_O_RDONLY, 0);
    if (source_fd < 0)
        return source_fd;
    destination_fd = fileXioOpen(
        destination, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (destination_fd < 0) {
        fileXioClose(source_fd);
        return destination_fd;
    }

    result = write_all(destination_fd, hdloader_kelf_header,
                       HDLOADER_KELF_HEADER_LEN);
    if (result >= 0) {
        *copied_bytes += HDLOADER_KELF_HEADER_LEN;
        report_copy_progress(work_done, HDLOADER_KELF_HEADER_LEN,
                             total_work, progress, progress_context);
    }
    remaining = source_size;
    while (result >= 0 && remaining != 0) {
        size_t request = remaining > sizeof(copy_buffer) ?
                         sizeof(copy_buffer) : (size_t)remaining;
        request = source_media_read_size(request);
        int read_result = fileXioRead(source_fd, copy_buffer, request);

        if (read_result <= 0) {
            result = read_result < 0 ? read_result : -EIO;
            break;
        }
        result = write_all(destination_fd, copy_buffer,
                           (size_t)read_result);
        if (result < 0)
            break;
        remaining -= (u64)read_result;
        *copied_bytes += (u64)read_result;
        report_copy_progress(work_done, (u64)read_result, total_work,
                             progress, progress_context);
    }
    padding_size = (u64)MAX_APP_ELF_BYTES - source_size;
    memset(copy_buffer, 0, sizeof(copy_buffer));
    while (result >= 0 && padding_size != 0) {
        size_t chunk = padding_size > sizeof(copy_buffer) ?
                       sizeof(copy_buffer) : (size_t)padding_size;

        result = write_all(destination_fd, copy_buffer, chunk);
        if (result < 0)
            break;
        padding_size -= chunk;
        *copied_bytes += chunk;
        report_copy_progress(work_done, chunk, total_work, progress,
                             progress_context);
    }
    if (result >= 0) {
        result = write_all(destination_fd, hdloader_kelf_footer,
                           HDLOADER_KELF_FOOTER_LEN);
        if (result >= 0) {
            *copied_bytes += HDLOADER_KELF_FOOTER_LEN;
            report_copy_progress(work_done, HDLOADER_KELF_FOOTER_LEN,
                                 total_work, progress, progress_context);
        }
    }
    {
        int close_result = fileXioClose(source_fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
        close_result = fileXioClose(destination_fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
    }
    if (result < 0)
        return result;

    destination_fd = fileXioOpen(destination, FIO_O_RDONLY, 0);
    if (destination_fd < 0)
        return destination_fd;
    result = read_exact(destination_fd, copy_buffer,
                        HDLOADER_KELF_HEADER_LEN);
    if (result >= 0 && memcmp(copy_buffer, hdloader_kelf_header,
                              HDLOADER_KELF_HEADER_LEN) != 0)
        result = -EIO;
    if (result >= 0)
        report_copy_progress(work_done, HDLOADER_KELF_HEADER_LEN,
                             total_work, progress, progress_context);

    sha256_init(&hash);
    remaining = source_size;
    while (result >= 0 && remaining != 0) {
        size_t request = remaining > sizeof(copy_buffer) ?
                         sizeof(copy_buffer) : (size_t)remaining;

        result = read_exact(destination_fd, copy_buffer, request);
        if (result < 0)
            break;
        sha256_update(&hash, copy_buffer, request);
        remaining -= request;
        report_copy_progress(work_done, request, total_work, progress,
                             progress_context);
    }
    sha256_final(&hash, digest);
    sha256_to_hex(digest, actual_hash);
    if (result >= 0 && !hash_equal(actual_hash, expected_hash))
        result = -EIO;

    padding_size = (u64)MAX_APP_ELF_BYTES - source_size;
    while (result >= 0 && padding_size != 0) {
        size_t index;
        size_t chunk = padding_size > sizeof(copy_buffer) ?
                       sizeof(copy_buffer) : (size_t)padding_size;

        result = read_exact(destination_fd, copy_buffer, chunk);
        if (result < 0)
            break;
        for (index = 0; index < chunk; ++index) {
            if (copy_buffer[index] != 0) {
                result = -EIO;
                break;
            }
        }
        padding_size -= chunk;
        report_copy_progress(work_done, chunk, total_work, progress,
                             progress_context);
    }
    if (result >= 0) {
        result = read_exact(destination_fd, copy_buffer,
                            HDLOADER_KELF_FOOTER_LEN);
        if (result >= 0 && memcmp(copy_buffer, hdloader_kelf_footer,
                                  HDLOADER_KELF_FOOTER_LEN) != 0)
            result = -EIO;
        if (result >= 0)
            report_copy_progress(work_done, HDLOADER_KELF_FOOTER_LEN,
                                 total_work, progress, progress_context);
    }
    if (result >= 0 && fileXioRead(destination_fd, copy_buffer, 1) != 0)
        result = -EIO;
    {
        int close_result = fileXioClose(destination_fd);
        if (result >= 0 && close_result < 0)
            result = close_result;
    }
    return result;
}

static int file_exists(const char *path)
{
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);

    if (fd < 0)
        return 0;
    fileXioClose(fd);
    return 1;
}

static int activate_staged_kelf(int updating)
{
    int result;

    fileXioRemove("pfs0:/EXECUTE.BAK");
    if (updating && file_exists("pfs0:/EXECUTE.KELF")) {
        result = fileXioRename("pfs0:/EXECUTE.KELF",
                               "pfs0:/EXECUTE.BAK");
        if (result < 0)
            return result;
    }
    result = fileXioRename("pfs0:/EXECUTE.NEW", "pfs0:/EXECUTE.KELF");
    if (result < 0) {
        if (updating && file_exists("pfs0:/EXECUTE.BAK"))
            fileXioRename("pfs0:/EXECUTE.BAK", "pfs0:/EXECUTE.KELF");
        return result;
    }
    return 0;
}

static void restore_previous_kelf(int updating)
{
    if (!updating || !file_exists("pfs0:/EXECUTE.BAK"))
        return;
    fileXioRemove("pfs0:/EXECUTE.KELF");
    fileXioRename("pfs0:/EXECUTE.BAK", "pfs0:/EXECUTE.KELF");
    fileXioSync("pfs0:", FXIO_WAIT);
}

static int build_metadata(const app_package_t *package,
                          char system_cnf[128], char icon_sys[512],
                          char info_sys[1024])
{
    int result;

    result = snprintf(system_cnf, 128,
        "BOOT2 = pfs:/EXECUTE.KELF\nVER = 1.01\nVMODE = NTSC\n"
        "HDDUNITPOWER = NICHDD\n");
    if (result < 0 || result >= 128)
        return -ENAMETOOLONG;
    result = snprintf(icon_sys, 512,
        "PS2X\n"
        "title0=%s\ntitle1=%s\n"
        "bgcola=0\nbgcol0=0,0,0\nbgcol1=0,0,0\n"
        "bgcol2=0,0,0\nbgcol3=0,0,0\n"
        "lightdir0=1.0,-1.0,1.0\nlightdir1=-1.0,1.0,-1.0\n"
        "lightdir2=0.0,0.0,0.0\nlightcolamb=64,64,64\n"
        "lightcol0=64,64,64\nlightcol1=16,16,16\n"
        "lightcol2=0,0,0\nuninstallmes0=Remove %s\n"
        "uninstallmes1=\nuninstallmes2=\n",
        package->title, package->subtitle, package->title);
    if (result < 0 || result >= 512)
        return -ENAMETOOLONG;
    result = snprintf(info_sys, 1024,
        "title = %s\ntitle_id = %s\ntitle_sub_id = 0\n"
        "release_date = 20260828\ndeveloper_id = Homebrew\n"
        "publisher_id = Homebrew\nnote = Installed by RepairBox.pl\n"
        "content_web = \nimage_topviewflag = 0\nimage_type = 0\n"
        "image_count = 1\nimage_viewsec = 600\n"
        "copyright_viewflag = 0\ncopyright_imgcount = 1\n"
        "genre = Utility\nparental_lock = 1\neffective_date = 0\n"
        "expire_date = 0\narea = J\nviolence_flag = 0\n"
        "content_type = 255\ncontent_subtype = 0\n",
        package->title, package->subtitle);
    if (result < 0 || result >= 1024)
        return -ENAMETOOLONG;
    return 0;
}

static int install_files(const app_preflight_t *preflight,
                         app_install_result_t *result,
                         app_progress_callback_t progress,
                         void *progress_context)
{
    char system_cnf[128];
    char icon_sys[512];
    char info_sys[1024];
    char installed_elf_hash[65];
    char verified_cover_hash[65];
    const void *cover_data = default_cover_png;
    size_t cover_size = size_default_cover_png;
    u64 total_work = (u64)KELF_LENGTH * 2u +
                     preflight->cover_size * 4u + 4096u;
    u64 work_done = 0;
    int value;

    value = build_metadata(&preflight->package, system_cnf,
                           icon_sys, info_sys);
    if (value < 0)
        return value;
    value = ensure_directory("pfs0:/res");
    if (value < 0)
        return value;
    progress(3, APP_STEP_COUNT, "COPY AND VERIFY APPLICATION", 0,
             progress_context);
    value = write_direct_kelf_verified(
        preflight->package.source_elf, "pfs0:/EXECUTE.NEW",
        preflight->elf_size, preflight->elf_sha256,
        &result->copied_bytes, &work_done, total_work, progress,
        progress_context, installed_elf_hash);
    if (value < 0)
        return value;
    value = fileXioSync("pfs0:", FXIO_WAIT);
    if (value < 0)
        return value;
    ++result->copied_files;
    ++result->verified_files;

#define WRITE_GENERATED(path, data, length) do { \
    value = write_memory_verified((path), (data), (length), \
                                  &result->copied_bytes); \
    if (value < 0) return value; \
    ++result->copied_files; \
    ++result->verified_files; \
} while (0)

    WRITE_GENERATED("pfs0:/SYSTEM.CNF", system_cnf, strlen(system_cnf));
    WRITE_GENERATED("pfs0:/icon.sys", icon_sys, strlen(icon_sys));
    WRITE_GENERATED("pfs0:/res/info.sys", info_sys, strlen(info_sys));
    if (preflight->package.cover_present) {
        value = copy_file_verified(preflight->package.source_cover,
            "pfs0:/res/jkt_001.png", preflight->cover_size,
            preflight->cover_sha256, &result->copied_bytes, &work_done,
            total_work, progress, progress_context, verified_cover_hash);
        if (value < 0)
            return value;
        ++result->copied_files;
        ++result->verified_files;
        value = copy_file_verified(preflight->package.source_cover,
            "pfs0:/res/jkt_002.png", preflight->cover_size,
            preflight->cover_sha256, &result->copied_bytes, &work_done,
            total_work, progress, progress_context, verified_cover_hash);
        if (value < 0)
            return value;
        ++result->copied_files;
        ++result->verified_files;
    } else {
        WRITE_GENERATED("pfs0:/res/jkt_001.png", cover_data, cover_size);
        WRITE_GENERATED("pfs0:/res/jkt_002.png", cover_data, cover_size);
    }
#undef WRITE_GENERATED

    /* Keep the previous executable active until every replacement file has
       been written and verified.  Only the final rename changes what the
       XMB will start. */
    value = fileXioSync("pfs0:", FXIO_WAIT);
    if (value < 0)
        return value;
    value = activate_staged_kelf(preflight->target_exists);
    if (value < 0)
        return value;
    value = fileXioSync("pfs0:", FXIO_WAIT);
    if (value < 0) {
        restore_previous_kelf(preflight->target_exists);
        return value;
    }
    if (copy_text(result->installed_elf_sha256,
                  sizeof(result->installed_elf_sha256),
                  installed_elf_hash) < 0) {
        restore_previous_kelf(preflight->target_exists);
        return -ENAMETOOLONG;
    }
    fileXioRemove("pfs0:/EXECUTE.BAK");
    fileXioRemove("pfs0:/APP.ELF");
    fileXioRemove("pfs0:/APP.BAK");
    fileXioRemove("pfs0:/EXECUTE.CNF");
    progress(3, APP_STEP_COUNT, "COPY AND VERIFY APPLICATION", 100,
             progress_context);
    return 0;
}

static void set_le32(unsigned char *output, u32 value)
{
    output[0] = (unsigned char)value;
    output[1] = (unsigned char)(value >> 8);
    output[2] = (unsigned char)(value >> 16);
    output[3] = (unsigned char)(value >> 24);
}

static int register_xmb_entry(const app_package_t *package,
                              u32 partition_start)
{
    unsigned char ppaa[SECTOR_BYTES] __attribute__((aligned(64)));
    unsigned char system_sector[SECTOR_BYTES] __attribute__((aligned(64)));
    unsigned char icon_sector[SECTOR_BYTES] __attribute__((aligned(64)));
    char system_cnf[128];
    char icon_sys[512];
    char info_sys[1024];
    u32 system_size;
    u32 icon_size;
    int result;

    result = build_metadata(package, system_cnf, icon_sys, info_sys);
    if (result < 0)
        return result;
    (void)info_sys;
    system_size = (u32)strlen(system_cnf);
    icon_size = (u32)strlen(icon_sys);
    memset(ppaa, 0, sizeof(ppaa));
    memset(system_sector, 0, sizeof(system_sector));
    memset(icon_sector, 0, sizeof(icon_sector));
    memcpy(system_sector, system_cnf, system_size);
    memcpy(icon_sector, icon_sys, icon_size);
    memcpy(ppaa, "PS2ICON3D", 9);
    set_le32(ppaa + 0x10, 0x0200u);
    set_le32(ppaa + 0x14, system_size);
    set_le32(ppaa + 0x18, 0x0400u);
    set_le32(ppaa + 0x1c, icon_size);

    result = write_sector(partition_start + 8u, ppaa);
    if (result < 0)
        return result;
    result = write_sector(partition_start + 9u, system_sector);
    if (result < 0)
        return result;
    result = write_sector(partition_start + 10u, icon_sector);
    if (result < 0)
        return result;
    result = read_sectors(partition_start + 8u, 1, readback_sector);
    if (result < 0 || memcmp(readback_sector, ppaa, SECTOR_BYTES) != 0)
        return result < 0 ? result : -EIO;
    result = read_sectors(partition_start + 9u, 1, readback_sector);
    if (result < 0 ||
        memcmp(readback_sector, system_sector, SECTOR_BYTES) != 0)
        return result < 0 ? result : -EIO;
    result = read_sectors(partition_start + 10u, 1, readback_sector);
    if (result < 0 ||
        memcmp(readback_sector, icon_sector, SECTOR_BYTES) != 0)
        return result < 0 ? result : -EIO;
    return 0;
}

void app_install(const app_preflight_t *preflight,
                 psx_revision_t revision,
                 app_install_result_t *result,
                 app_progress_callback_t progress,
                 void *progress_context)
{
    static const int pfs_format_args[1] = {APP_PFS_ZONE_SIZE};
    char blockdev[80];
    int value;
    int mount_result;
    int mount_owned = 0;

    (void)revision;
    memset(result, 0, sizeof(*result));
    memcpy(&result->package, &preflight->package, sizeof(result->package));
    if (!preflight->valid || !preflight->required_partitions_valid) {
        set_install_failure(result, 1, -EPERM, "preflight", "package/layout");
        return;
    }
    result->target_was_updated = preflight->target_exists;
    snprintf(blockdev, sizeof(blockdev), "hdd0:%s",
             preflight->package.partition_name);

    progress(1, APP_STEP_COUNT, "PREPARE APPLICATION PARTITION", 0,
             progress_context);
    if (!preflight->target_exists) {
        value = create_target_partition(&preflight->package);
        if (value < 0) {
            set_install_failure(result, 1, value, "create partition",
                                preflight->package.partition_name);
            return;
        }
        result->target_was_created = 1;
    }
    value = refresh_target(&preflight->package, result);
    if (value < 0) {
        set_install_failure(result, 1, value, "validate APA target",
                            preflight->package.partition_name);
        return;
    }
    progress(1, APP_STEP_COUNT, "PREPARE APPLICATION PARTITION", 100,
             progress_context);

    progress(2, APP_STEP_COUNT, "PREPARE PFS FILESYSTEM", 0,
             progress_context);
    fileXioUmount("pfs0:");
    mount_result = fileXioMount("pfs0:", blockdev, FIO_MT_RDWR);
    if (mount_result < 0) {
        value = fileXioFormat("pfs:", blockdev,
                              (const char *)&pfs_format_args,
                              sizeof(pfs_format_args));
        if (value < 0) {
            set_install_failure(result, 2, value, "format app PFS",
                                preflight->package.partition_name);
            return;
        }
        result->target_was_reformatted = 1;
        mount_result = fileXioMount("pfs0:", blockdev, FIO_MT_RDWR);
    }
    if (mount_result < 0) {
        set_install_failure(result, 2, mount_result, "mount app PFS",
                            preflight->package.partition_name);
        return;
    }
    mount_owned = 1;
    progress(2, APP_STEP_COUNT, "PREPARE PFS FILESYSTEM", 100,
             progress_context);

    value = install_files(preflight, result, progress, progress_context);
    if (value < 0) {
        set_install_failure(result, 3, value, "copy / verify application",
                            preflight->package.elf_name);
        goto cleanup;
    }
    progress(4, APP_STEP_COUNT, "FLUSH AND CLOSE FILESYSTEM", 0,
             progress_context);
    value = fileXioSync("pfs0:", FXIO_WAIT);
    if (value < 0) {
        set_install_failure(result, 4, value, "sync PFS",
                            preflight->package.partition_name);
        goto cleanup;
    }
    fileXioRemove("pfs0:/APP.BAK");
    value = fileXioUmount("pfs0:");
    mount_owned = 0;
    if (value < 0) {
        set_install_failure(result, 4, value, "unmount PFS",
                            preflight->package.partition_name);
        return;
    }
    progress(4, APP_STEP_COUNT, "FLUSH AND CLOSE FILESYSTEM", 100,
             progress_context);

    progress(5, APP_STEP_COUNT, "REGISTER APPLICATION IN XMB", 0,
             progress_context);
    value = register_xmb_entry(&preflight->package, result->target_start);
    if (value < 0) {
        set_install_failure(result, 5, value, "write/read XMB header",
                            preflight->package.partition_name);
        return;
    }
    progress(5, APP_STEP_COUNT, "REGISTER APPLICATION IN XMB", 100,
             progress_context);

    progress(6, APP_STEP_COUNT, "FINAL TARGET VERIFICATION", 0,
             progress_context);
    value = validate_target_header(&preflight->package,
                                   result->target_start,
                                   result->target_length);
    if (value < 0 || result->copied_files != APP_INSTALLED_FILE_COUNT ||
        result->verified_files != APP_INSTALLED_FILE_COUNT ||
        !hash_equal(result->installed_elf_sha256,
                    preflight->elf_sha256)) {
        set_install_failure(result, 6, value < 0 ? value : -EIO,
                            "final verification",
                            preflight->package.partition_name);
        return;
    }
    progress(6, APP_STEP_COUNT, "FINAL TARGET VERIFICATION", 100,
             progress_context);
    result->success = 1;
    return;

cleanup:
    if (mount_owned)
        fileXioUmount("pfs0:");
}
