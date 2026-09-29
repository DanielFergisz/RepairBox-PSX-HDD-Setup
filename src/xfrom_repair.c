#include <delaythread.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "sha256.h"
#include "source_media.h"
#include "xfrom_repair.h"

#define XFROM_DEVICE_ROOT "xfrom0:/"
#define XFROM_SYSTEM_ROOT "xfrom0:/BIEXEC-SYSTEM"
#define XFROM_DESTINATION_PREFIX "xfrom0:/BIEXEC-SYSTEM/"
#define XFROM_FILE_LIMIT (8u * 1024u * 1024u)
/* Match fileXio's default IOP transfer buffer.  Smaller requests multiply
 * synchronous EE<->IOP RPC round trips without improving XFROM safety. */
#define XFROM_IO_SIZE (16u * 1024u)
#define XFROM_IO_ATTEMPTS 2u
#define XFROM_CLEANUP_MAX_FILES 64u

typedef struct xfrom_cleanup_plan {
    char paths[XFROM_CLEANUP_MAX_FILES][XFROM_REPAIR_PATH_SIZE];
    u32 count;
} xfrom_cleanup_plan_t;

typedef struct known_xfrom_file {
    const char *name;
    u32 size;
    u8 sha256[32];
    int deferred_activation;
} known_xfrom_file_t;

static unsigned char io_buffer[XFROM_IO_SIZE] __attribute__((aligned(64)));

static void report_progress(xfrom_progress_callback_t progress,
                            void *context, xfrom_progress_phase_t phase,
                            const char *path, u32 file_index,
                            u32 file_count, u32 completed, u32 total)
{
    if (progress != NULL)
        progress(phase, path, file_index, file_count, completed, total,
                 context);
}

static const known_xfrom_file_t psx1_files[] = {
    {"INSTALL.ID", 0u,
     {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
      0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
      0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55}, 0},
    {"osdmain.dat", 6144000u,
     {0x0a, 0x5e, 0x6a, 0xd1, 0xae, 0xef, 0x76, 0x8b,
      0x74, 0x92, 0xdc, 0xc4, 0xd2, 0x59, 0x5c, 0xb0,
      0x2b, 0xa4, 0x9e, 0x3e, 0x1d, 0xde, 0x1d, 0x32,
      0x67, 0x7f, 0xd2, 0x99, 0x6d, 0xaf, 0x93, 0x05}, 0},
    {"osdmain.mod", 2048u,
     {0x9d, 0xcd, 0x3c, 0xe0, 0xde, 0x29, 0x2d, 0x21,
      0xc0, 0x66, 0x1e, 0x5d, 0x87, 0xe8, 0xd3, 0x64,
      0x63, 0x44, 0xa7, 0x2f, 0xd9, 0x1b, 0x94, 0xfd,
      0x8d, 0x94, 0x24, 0x19, 0x52, 0x4b, 0xbb, 0x79}, 0},
    {"xosdmain.elf", 423760u,
     {0x16, 0x40, 0x4c, 0x54, 0x51, 0x04, 0x2d, 0xfa,
      0xc3, 0x3c, 0xff, 0xbd, 0x85, 0xd9, 0x55, 0x4f,
      0xf3, 0xab, 0x4e, 0x54, 0x9f, 0x39, 0xe5, 0x0d,
      0xa5, 0x92, 0x12, 0xe4, 0x1e, 0xf6, 0xd0, 0x3a}, 0},
};

static const known_xfrom_file_t psx2_files[] = {
    {"bootflag.bak", 512u,
     {0x61, 0xf0, 0x4f, 0xc7, 0x09, 0x58, 0x20, 0xc8,
      0x27, 0x41, 0xc9, 0xeb, 0x6e, 0x82, 0x87, 0xd0,
      0x0a, 0x42, 0x8d, 0xc5, 0xd7, 0x4d, 0x34, 0x47,
      0xea, 0x44, 0x9f, 0x80, 0xd9, 0xae, 0x9c, 0x9f}, 0},
    /* bootflag.txt is checked here but remains the final activation write. */
    {"bootflag.txt", 512u,
     {0x61, 0xf0, 0x4f, 0xc7, 0x09, 0x58, 0x20, 0xc8,
      0x27, 0x41, 0xc9, 0xeb, 0x6e, 0x82, 0x87, 0xd0,
      0x0a, 0x42, 0x8d, 0xc5, 0xd7, 0x4d, 0x34, 0x47,
      0xea, 0x44, 0x9f, 0x80, 0xd9, 0xae, 0x9c, 0x9f}, 1},
    {"INSTALL.ID", 0u,
     {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
      0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
      0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55}, 0},
    {"xosdmain.elf", 478688u,
     {0xb2, 0x7c, 0x00, 0x86, 0x5a, 0x78, 0x59, 0x1c,
      0x99, 0x37, 0x67, 0x58, 0x6a, 0x87, 0x4c, 0x90,
      0x92, 0xc8, 0xc6, 0x38, 0x34, 0xd4, 0x72, 0x4c,
      0xcb, 0xe0, 0xee, 0xa6, 0xd1, 0x0e, 0xca, 0xd4}, 0},
};

static u32 elapsed_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static void fail(xfrom_repair_result_t *result, const char *operation,
                 const char *path, int return_value)
{
    if (result->failure_return < 0)
        return;
    result->failure_return = return_value < 0 ? return_value : -EIO;
    snprintf(result->failure_operation, sizeof(result->failure_operation),
             "%s", operation);
    snprintf(result->failure_path, sizeof(result->failure_path), "%.191s",
             path != NULL ? path : "");
}

static const char *source_root(xfrom_repair_revision_t revision)
{
    return revision == XFROM_REPAIR_PSX1
               ? source_media_psx1_xfrom_root()
               : source_media_psx2_xfrom_root();
}

static int build_source_path(char *output, size_t output_size,
                             const xfrom_repair_result_t *result,
                             const xfrom_repair_entry_t *entry,
                             int nested)
{
    const char *name = strrchr(entry->path, '/');
    int length;

    name = name != NULL ? name + 1 : entry->path;
    length = nested
        ? snprintf(output, output_size, "%s/%s",
                   source_root(result->revision), entry->path)
        : snprintf(output, output_size, "%s/%s",
                   source_root(result->revision), name);
    return length > 0 && (size_t)length < output_size
               ? 0 : -ENAMETOOLONG;
}

static int build_destination_path(char *output, size_t output_size,
                                  const xfrom_repair_entry_t *entry)
{
    const char *name = strrchr(entry->path, '/');
    int length;

    name = name != NULL ? name + 1 : entry->path;
    length = snprintf(output, output_size, "%s%s",
                      XFROM_DESTINATION_PREFIX, name);
    return length > 0 && (size_t)length < output_size
               ? 0 : -ENAMETOOLONG;
}

static int hash_path(const char *path, int source, u8 digest[32], u32 *size,
                     xfrom_progress_callback_t progress,
                     void *progress_context,
                     xfrom_progress_phase_t progress_phase,
                     const char *progress_path, u32 file_index,
                     u32 file_count, u32 expected_size)
{
    sha256_context_t hash;
    u32 total = 0;
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    int close_result;

    if (fd < 0)
        return fd;
    report_progress(progress, progress_context, progress_phase,
                    progress_path, file_index, file_count, 0u,
                    expected_size);
    sha256_init(&hash);
    for (;;) {
        size_t request = source ? source_media_read_size(sizeof(io_buffer))
                                : sizeof(io_buffer);
        int amount = fileXioRead(fd, io_buffer, request);

        if (amount < 0) {
            fileXioClose(fd);
            return amount;
        }
        if (amount == 0)
            break;
        if (total > XFROM_FILE_LIMIT - (u32)amount) {
            fileXioClose(fd);
            return -EFBIG;
        }
        total += (u32)amount;
        sha256_update(&hash, io_buffer, (size_t)amount);
        report_progress(progress, progress_context, progress_phase,
                        progress_path, file_index, file_count, total,
                        expected_size);
    }
    close_result = fileXioClose(fd);
    if (close_result < 0)
        return close_result;
    sha256_final(&hash, digest);
    *size = total;
    return 0;
}

static int populate_entries(xfrom_repair_result_t *result)
{
    const known_xfrom_file_t *known;
    unsigned int count;
    unsigned int index;

    if (result->revision == XFROM_REPAIR_PSX1) {
        known = psx1_files;
        count = sizeof(psx1_files) / sizeof(psx1_files[0]);
    } else if (result->revision == XFROM_REPAIR_PSX2) {
        known = psx2_files;
        count = sizeof(psx2_files) / sizeof(psx2_files[0]);
    } else {
        return -EINVAL;
    }
    if (count > XFROM_REPAIR_MAX_FILES)
        return -E2BIG;
    for (index = 0; index < count; ++index) {
        xfrom_repair_entry_t *entry = &result->files[index];

        snprintf(entry->path, sizeof(entry->path),
                 "BIEXEC-SYSTEM/%s", known[index].name);
        memcpy(entry->expected_sha256, known[index].sha256, 32u);
        entry->size = known[index].size;
        entry->deferred_activation = known[index].deferred_activation;
    }
    result->file_count = count;
    return 0;
}

void xfrom_repair_initialize(xfrom_repair_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->xfrom_root_open_result = -1;
    result->xfrom_root_close_result = -1;
    result->xfrom_partition_open_result = -1;
    result->xfrom_partition_close_result = -1;
    result->failure_return = 0;
}

void xfrom_repair_validate_source(xfrom_repair_result_t *result,
                                  xfrom_repair_revision_t revision,
                                  xfrom_progress_callback_t progress,
                                  void *progress_context)
{
    unsigned int index;
    int populate_result;

    xfrom_repair_initialize(result);
    result->revision = revision;
    populate_result = populate_entries(result);
    if (populate_result < 0) {
        fail(result, "select_known_file_set", "XFROM", populate_result);
        return;
    }
    for (index = 0; index < result->file_count; ++index) {
        xfrom_repair_entry_t *entry = &result->files[index];
        char path[384];
        u8 digest[32];
        u32 size = 0;
        int hash_result;

        if (build_source_path(path, sizeof(path), result, entry, 1) < 0) {
            fail(result, "source_path", entry->path, -ENAMETOOLONG);
            return;
        }
        hash_result = hash_path(path, 1, digest, &size, progress,
                                progress_context,
                                XFROM_PROGRESS_VALIDATE_BACKUP,
                                entry->path, index + 1u,
                                result->file_count, entry->size);
        if (hash_result < 0) {
            if (build_source_path(path, sizeof(path), result, entry, 0) < 0) {
                fail(result, "source_path", entry->path, -ENAMETOOLONG);
                return;
            }
            hash_result = hash_path(path, 1, digest, &size, progress,
                                    progress_context,
                                    XFROM_PROGRESS_VALIDATE_BACKUP,
                                    entry->path, index + 1u,
                                    result->file_count, entry->size);
            entry->source_nested = 0;
        } else {
            entry->source_nested = 1;
        }
        entry->source_valid = hash_result == 0 && size == entry->size &&
            memcmp(digest, entry->expected_sha256, 32u) == 0;
        if (!entry->source_valid) {
            fail(result, "validate_known_source", entry->path,
                 hash_result < 0 ? hash_result : -EILSEQ);
            return;
        }
        if (result->total_bytes > 0xffffffffu - entry->size) {
            fail(result, "package_size", entry->path, -EFBIG);
            return;
        }
        result->total_bytes += entry->size;
    }
    result->core_files_valid = 1;
    result->xfrom_root_open_result = fileXioDopen(XFROM_DEVICE_ROOT);
    if (result->xfrom_root_open_result >= 0)
        result->xfrom_root_close_result =
            fileXioDclose(result->xfrom_root_open_result);
    result->xfrom_root_accessible =
        result->xfrom_root_open_result >= 0 &&
        result->xfrom_root_close_result >= 0;
    if (!result->xfrom_root_accessible) {
        fail(result, "open_xfrom_root", XFROM_DEVICE_ROOT,
             result->xfrom_root_open_result < 0
                 ? result->xfrom_root_open_result
                 : result->xfrom_root_close_result);
        return;
    }
    result->xfrom_partition_open_result = fileXioDopen(XFROM_SYSTEM_ROOT);
    if (result->xfrom_partition_open_result >= 0)
        result->xfrom_partition_close_result =
            fileXioDclose(result->xfrom_partition_open_result);
    result->xfrom_partition_accessible =
        result->xfrom_partition_open_result >= 0 &&
        result->xfrom_partition_close_result >= 0;
    result->xfrom_partition_missing =
        result->xfrom_partition_open_result < 0;
    if (result->xfrom_partition_open_result >= 0 &&
        result->xfrom_partition_close_result < 0) {
        fail(result, "close_biexec", XFROM_SYSTEM_ROOT,
             result->xfrom_partition_close_result);
        return;
    }
    result->source_valid = 1;
}

static int ensure_system_root(xfrom_repair_result_t *result)
{
    int fd = fileXioDopen(XFROM_SYSTEM_ROOT);
    int operation_result;

    if (fd >= 0) {
        operation_result = fileXioDclose(fd);
        if (operation_result < 0)
            return operation_result;
        result->xfrom_partition_accessible = 1;
        result->xfrom_partition_missing = 0;
        return 0;
    }
    operation_result = fileXioMkdir(XFROM_SYSTEM_ROOT, 0777);
    if (operation_result < 0)
        return operation_result;
    result->xfrom_partition_created = 1;
    fd = fileXioDopen(XFROM_SYSTEM_ROOT);
    if (fd < 0)
        return fd;
    operation_result = fileXioDclose(fd);
    if (operation_result < 0)
        return operation_result;
    result->xfrom_partition_accessible = 1;
    result->xfrom_partition_missing = 0;
    return 0;
}

static int known_destination_name(const xfrom_repair_result_t *result,
                                  const char *name)
{
    unsigned int index;

    for (index = 0; index < result->file_count; ++index) {
        const char *known = strrchr(result->files[index].path, '/');

        known = known != NULL ? known + 1 : result->files[index].path;
        if (strcmp(known, name) == 0)
            return 1;
    }
    return 0;
}

static int safe_cleanup_name(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
        strchr(name, '/') == NULL && strchr(name, '\\') == NULL &&
        strchr(name, ':') == NULL;
}

static int build_cleanup_plan(xfrom_repair_result_t *result,
                              xfrom_cleanup_plan_t *plan)
{
    iox_dirent_t entry;
    int fd = fileXioDopen(XFROM_SYSTEM_ROOT);
    int read_result = 0;

    memset(plan, 0, sizeof(*plan));
    result->cleanup_entries_scanned = 0;
    if (fd < 0)
        return fd;
    for (;;) {
        int length;

        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        if (memchr(entry.name, '\0', sizeof(entry.name)) == NULL) {
            read_result = -ENAMETOOLONG;
            fail(result, "unterminated_xfrom_entry", XFROM_SYSTEM_ROOT,
                 read_result);
            break;
        }
        if (strcmp(entry.name, ".") == 0 ||
            strcmp(entry.name, "..") == 0)
            continue;
        ++result->cleanup_entries_scanned;
        if (!safe_cleanup_name(entry.name)) {
            read_result = -EINVAL;
            fail(result, "unsafe_xfrom_entry", entry.name, read_result);
            break;
        }
        if (FIO_S_ISDIR(entry.stat.mode)) {
            read_result = -EISDIR;
            fail(result, "unexpected_xfrom_directory", entry.name,
                 read_result);
            break;
        }
        if (known_destination_name(result, entry.name))
            continue;
        if (plan->count >= XFROM_CLEANUP_MAX_FILES) {
            read_result = -E2BIG;
            fail(result, "too_many_xfrom_extras", entry.name,
                 read_result);
            break;
        }
        length = snprintf(plan->paths[plan->count],
                          sizeof(plan->paths[plan->count]), "%s/%s",
                          XFROM_SYSTEM_ROOT, entry.name);
        if (length <= 0 ||
            (size_t)length >= sizeof(plan->paths[plan->count])) {
            read_result = -ENAMETOOLONG;
            fail(result, "xfrom_cleanup_path", entry.name, read_result);
            break;
        }
        ++plan->count;
    }
    {
        int close_result = fileXioDclose(fd);

        if (read_result >= 0 && close_result < 0)
            read_result = close_result;
    }
    if (read_result < 0 && result->failure_return >= 0)
        fail(result, "scan_xfrom_cleanup", XFROM_SYSTEM_ROOT, read_result);
    if (read_result < 0)
        return read_result;
    result->cleanup_candidates = plan->count;
    return 0;
}

static int cleanup_system_root(xfrom_repair_result_t *result,
                               xfrom_progress_callback_t progress,
                               void *progress_context)
{
    xfrom_cleanup_plan_t plan;
    unsigned int index;
    int operation_result;

    report_progress(progress, progress_context, XFROM_PROGRESS_SCAN_CLEANUP,
                    XFROM_SYSTEM_ROOT, 0u, 0u, 0u, 1u);
    operation_result = build_cleanup_plan(result, &plan);

    if (operation_result < 0)
        return operation_result;
    report_progress(progress, progress_context, XFROM_PROGRESS_SCAN_CLEANUP,
                    XFROM_SYSTEM_ROOT, 0u, 0u, 1u, 1u);
    report_progress(progress, progress_context, XFROM_PROGRESS_REMOVE_EXTRAS,
                    XFROM_SYSTEM_ROOT, 0u, 0u, 0u, plan.count);
    for (index = 0; index < plan.count; ++index) {
        operation_result = fileXioRemove(plan.paths[index]);
        if (operation_result < 0) {
            fail(result, "remove_extra_xfrom_file", plan.paths[index],
                 operation_result);
            return operation_result;
        }
        ++result->cleanup_removed;
        report_progress(progress, progress_context,
                        XFROM_PROGRESS_REMOVE_EXTRAS, plan.paths[index],
                        0u, 0u, index + 1u, plan.count);
    }
    report_progress(progress, progress_context,
                    XFROM_PROGRESS_VERIFY_CLEANUP, XFROM_SYSTEM_ROOT,
                    0u, 0u, 0u, 1u);
    operation_result = build_cleanup_plan(result, &plan);
    if (operation_result < 0)
        return operation_result;
    if (plan.count != 0u) {
        fail(result, "verify_xfrom_cleanup", XFROM_SYSTEM_ROOT, -EIO);
        return -EIO;
    }
    result->cleanup_valid = 1;
    report_progress(progress, progress_context,
                    XFROM_PROGRESS_VERIFY_CLEANUP, XFROM_SYSTEM_ROOT,
                    0u, 0u, 1u, 1u);
    return 0;
}

static int load_verified_source(const xfrom_repair_result_t *result,
                                const xfrom_repair_entry_t *entry,
                                unsigned char **bytes,
                                xfrom_progress_callback_t progress,
                                void *progress_context, u32 file_index)
{
    char path[384];
    sha256_context_t hash;
    u8 digest[32];
    unsigned char *buffer;
    u32 offset = 0;
    int fd;

    if (build_source_path(path, sizeof(path), result, entry,
                          entry->source_nested) < 0)
        return -ENAMETOOLONG;
    buffer = memalign(64, entry->size > 0u ? entry->size : 64u);
    if (buffer == NULL)
        return -ENOMEM;
    fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    if (fd < 0) {
        free(buffer);
        return fd;
    }
    report_progress(progress, progress_context,
                    XFROM_PROGRESS_RELOAD_BACKUP, entry->path,
                    file_index, result->file_count, 0u, entry->size);
    sha256_init(&hash);
    while (offset < entry->size) {
        size_t request = entry->size - offset;
        int amount;

        if (request > XFROM_IO_SIZE)
            request = XFROM_IO_SIZE;
        request = source_media_read_size(request);
        amount = fileXioRead(fd, buffer + offset, request);
        if (amount <= 0) {
            fileXioClose(fd);
            free(buffer);
            return amount < 0 ? amount : -EIO;
        }
        offset += (u32)amount;
        sha256_update(&hash, buffer + offset - (u32)amount, (size_t)amount);
        report_progress(progress, progress_context,
                        XFROM_PROGRESS_RELOAD_BACKUP, entry->path,
                        file_index, result->file_count, offset,
                        entry->size);
    }
    {
        unsigned char extra;
        int extra_result = fileXioRead(fd, &extra, 1u);
        int close_result = fileXioClose(fd);

        if (extra_result != 0 || close_result < 0) {
            free(buffer);
            return extra_result < 0 ? extra_result : -EIO;
        }
    }
    sha256_final(&hash, digest);
    if (memcmp(digest, entry->expected_sha256, 32u) != 0) {
        free(buffer);
        return -EILSEQ;
    }
    *bytes = buffer;
    return 0;
}

static int write_verified_destination(xfrom_repair_entry_t *entry,
                                      const unsigned char *bytes,
                                      xfrom_progress_callback_t progress,
                                      void *progress_context,
                                      u32 file_index, u32 file_count)
{
    char path[384];
    u32 offset = 0;
    int fd;
    u8 digest[32];
    u32 size;
    int hash_result;

    if (build_destination_path(path, sizeof(path), entry) < 0)
        return -ENAMETOOLONG;
    entry->write_attempted = 1;
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    report_progress(progress, progress_context, XFROM_PROGRESS_WRITE_TARGET,
                    entry->path, file_index, file_count, 0u, entry->size);
    while (offset < entry->size) {
        u32 request = entry->size - offset;
        int amount;

        if (request > XFROM_IO_SIZE)
            request = XFROM_IO_SIZE;
        amount = fileXioWrite(fd, bytes + offset, request);
        if (amount <= 0) {
            fileXioClose(fd);
            return amount < 0 ? amount : -EIO;
        }
        offset += (u32)amount;
        report_progress(progress, progress_context,
                        XFROM_PROGRESS_WRITE_TARGET, entry->path,
                        file_index, file_count, offset, entry->size);
    }
    if (fileXioClose(fd) < 0)
        return -EIO;
    hash_result = hash_path(path, 0, digest, &size, progress,
                            progress_context, XFROM_PROGRESS_VERIFY_TARGET,
                            entry->path, file_index, file_count,
                            entry->size);
    entry->destination_valid = hash_result == 0 && size == entry->size &&
        memcmp(digest, entry->expected_sha256, 32u) == 0;
    return entry->destination_valid ? 0
                                    : (hash_result < 0 ? hash_result : -EILSEQ);
}

int xfrom_repair_execute(xfrom_repair_result_t *result,
                         int storage_valid, int session_confirmed,
                         xfrom_progress_callback_t progress,
                         void *progress_context)
{
    unsigned int index;
    u64 start;

    int root_result;

    if (!storage_valid || !session_confirmed || !result->source_valid ||
        !result->xfrom_root_accessible)
        return -EPERM;
    result->write_attempted = 1;
    start = GetTimerSystemTime();
    report_progress(progress, progress_context, XFROM_PROGRESS_PREPARE_TARGET,
                    XFROM_SYSTEM_ROOT, 0u, 0u, 0u, 1u);
    root_result = ensure_system_root(result);
    if (root_result < 0) {
        fail(result, "create_or_open_biexec", XFROM_SYSTEM_ROOT,
             root_result);
        result->duration_ms = elapsed_ms(start, GetTimerSystemTime());
        return root_result;
    }
    report_progress(progress, progress_context, XFROM_PROGRESS_PREPARE_TARGET,
                    XFROM_SYSTEM_ROOT, 0u, 0u, 1u, 1u);
    root_result = cleanup_system_root(result, progress, progress_context);
    if (root_result < 0) {
        result->duration_ms = elapsed_ms(start, GetTimerSystemTime());
        return root_result;
    }
    for (index = 0; index < result->file_count; ++index) {
        xfrom_repair_entry_t *entry = &result->files[index];
        unsigned int attempt;
        int operation_result = -EIO;

        if (entry->deferred_activation) {
            ++result->files_deferred;
            continue;
        }
        for (attempt = 0; attempt < XFROM_IO_ATTEMPTS; ++attempt) {
            unsigned char *bytes = NULL;
            int load_result = load_verified_source(
                result, entry, &bytes, progress, progress_context,
                index + 1u);

            if (load_result < 0) {
                operation_result = load_result;
            } else {
                operation_result = write_verified_destination(
                    entry, bytes, progress, progress_context,
                    index + 1u, result->file_count);
                free(bytes);
            }
            if (operation_result == 0 || operation_result != -EIO)
                break;
            DelayThread(250000);
        }
        if (operation_result < 0) {
            fail(result,
                 entry->write_attempted ? "write_verify_xfrom"
                                        : "reload_source_file",
                 entry->path, operation_result);
            result->duration_ms = elapsed_ms(start, GetTimerSystemTime());
            return operation_result;
        }
        ++result->files_written;
    }
    result->repair_valid =
        result->files_written + result->files_preserved +
        result->files_deferred == result->file_count;
    result->duration_ms = elapsed_ms(start, GetTimerSystemTime());
    return result->repair_valid ? 0 : -EIO;
}
