#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <fileXio_rpc.h>
#include <iox_stat.h>

#include "installer.h"
#include "sha256.h"
#include "ui.h"

#define COPY_BUFFER_SIZE (64u * 1024u)
#define PROGRESS_REFRESH_MS 500u
#define PROGRESS_LINE_WIDTH 54
typedef struct child_name {
    char name[256];
} child_name_t;

static unsigned char copy_buffer[COPY_BUFFER_SIZE] __attribute__((aligned(64)));
static u64 progress_start_time;
static u32 progress_last_draw_ms;
static int progress_screen_ready;

typedef struct installer_partition_config {
    const char *name;
    const char *blockdev;
    const char *mountpoint;
    const char *root_prefix;
    int allow_preexisting_mount;
} installer_partition_config_t;

static const installer_partition_config_t
psx2_partitions[INSTALLER_PARTITION_COUNT] = {
    {"__net", "hdd0:__net", "pfs0:", "pfs0:", 0},
    {"__system", "hdd0:__system", "pfs0:", "pfs0:", 0},
    {"__sysconf", "hdd0:__sysconf", "pfs0:", "pfs0:", 0},
    {"__common", "hdd0:__common", "pfs0:", "pfs0:", 0},
    {"__xdata", "dvr_hdd0:__xdata", "dvr_pfs1:", "dvr_pfs1:", 1},
    {"__xcontents", "dvr_hdd0:__xcontents", "dvr_pfs0:", "dvr_pfs0:", 1},
};

static const installer_partition_config_t
psx1_partitions[INSTALLER_PSX1_PARTITION_COUNT] = {
    {"__net", "hdd0:__net", "pfs0:", "pfs0:", 0},
    {"__system", "hdd0:__system", "pfs0:", "pfs0:", 0},
    {"__sysconf", "hdd0:__sysconf", "pfs0:", "pfs0:", 0},
    {"__common", "hdd0:__common", "pfs0:", "pfs0:", 0},
};

static void set_failure(installer_result_t *result, int return_value,
                        const char *partition, const char *path,
                        const char *operation)
{
    if (result->failure_return != 0)
        return;
    result->failure_return = return_value != 0 ? return_value : -EIO;
    snprintf(result->failure_partition, sizeof(result->failure_partition),
             "%s", partition != NULL ? partition : "");
    snprintf(result->failure_path, sizeof(result->failure_path), "%s",
             path != NULL ? path : "");
    snprintf(result->failure_operation, sizeof(result->failure_operation),
             "%s", operation != NULL ? operation : "unknown");
}

static void initialize_for_revision(
    installer_result_t *result, const char *source_root,
    unsigned int partition_count,
    const installer_partition_config_t *partitions,
    unsigned int progress_step, unsigned int progress_step_count,
    const char *progress_operation)
{
    unsigned int index;

    memset(result, 0, sizeof(*result));
    result->source_root = source_root;
    result->partition_count = partition_count;
    result->progress_step = progress_step;
    result->progress_step_count = progress_step_count;
    result->progress_operation = progress_operation;
    result->preflight_failure_mask = 0;
    result->source_root_open_result = -1;
    result->source_root_final_read_result = -1;
    result->source_root_close_result = -1;
    result->all_unmounted_cleanly = 1;
    for (index = 0; index < partition_count; ++index) {
        installer_partition_result_t *partition = &result->partitions[index];

        partition->name = partitions[index].name;
        partition->blockdev = partitions[index].blockdev;
        partition->mountpoint = partitions[index].mountpoint;
        partition->root_prefix = partitions[index].root_prefix;
        partition->allow_preexisting_mount =
            partitions[index].allow_preexisting_mount;
        partition->source_open_result = -1;
        partition->source_final_read_result = -1;
        partition->source_close_result = -1;
        partition->mount_result = -1;
        partition->mount_probe_result = -1;
        partition->mount_probe_close_result = -1;
        partition->umount_result = -1;
    }
}

void installer_initialize(installer_result_t *result,
                          const storage_diagnostics_t *diagnostics)
{
    (void)diagnostics;
    initialize_for_revision(result, INSTALLER_SOURCE_ROOT,
                            INSTALLER_PARTITION_COUNT, psx2_partitions,
                            11u, 13u, "COPY SYSTEM/DVR FILES");
}

void installer_initialize_psx1(installer_result_t *result)
{
    initialize_for_revision(result, INSTALLER_PSX1_SOURCE_ROOT,
                            INSTALLER_PSX1_PARTITION_COUNT,
                            psx1_partitions,
                            7u, 8u, "COPY AND VERIFY PACKAGE");
}

void installer_release(installer_result_t *result)
{
    size_t index;

    for (index = 0; index < result->item_count; ++index)
        free(result->items[index].relative_path);
    free(result->items);
    result->items = NULL;
    result->item_count = 0;
    result->item_capacity = 0;
    for (index = 0; index < result->unknown_top_level_count; ++index)
        free(result->unknown_top_level_names[index]);
    free(result->unknown_top_level_names);
    result->unknown_top_level_names = NULL;
    result->unknown_top_level_count = 0;
    result->unknown_top_level_capacity = 0;
}

static char *duplicate_string(const char *value)
{
    size_t length = strlen(value) + 1u;
    char *copy = malloc(length);

    if (copy != NULL)
        memcpy(copy, value, length);
    return copy;
}

static int append_item(installer_result_t *result, unsigned int partition,
                       installer_item_type_t type, const char *relative_path,
                       u64 size)
{
    installer_source_item_t *item;

    if (result->item_count == result->item_capacity) {
        size_t capacity = result->item_capacity == 0
                              ? 128u
                              : result->item_capacity * 2u;
        installer_source_item_t *resized =
            realloc(result->items, capacity * sizeof(*resized));

        if (resized == NULL)
            return -ENOMEM;
        result->items = resized;
        result->item_capacity = capacity;
    }
    item = &result->items[result->item_count];
    item->relative_path = duplicate_string(relative_path);
    if (item->relative_path == NULL)
        return -ENOMEM;
    item->partition_index = partition;
    item->type = type;
    item->size = size;
    ++result->item_count;
    return 0;
}

static int append_unknown(installer_result_t *result, const char *name)
{
    char *copy;

    if (result->unknown_top_level_count == result->unknown_top_level_capacity) {
        size_t capacity = result->unknown_top_level_capacity == 0
                              ? 8u
                              : result->unknown_top_level_capacity * 2u;
        char **resized = realloc(result->unknown_top_level_names,
                                 capacity * sizeof(*resized));

        if (resized == NULL)
            return -ENOMEM;
        result->unknown_top_level_names = resized;
        result->unknown_top_level_capacity = capacity;
    }
    copy = duplicate_string(name);
    if (copy == NULL)
        return -ENOMEM;
    result->unknown_top_level_names[result->unknown_top_level_count++] = copy;
    return 0;
}

static int partition_index_from_name(const installer_result_t *result,
                                     const char *name)
{
    unsigned int index;

    for (index = 0; index < result->partition_count; ++index) {
        if (strcmp(name, result->partitions[index].name) == 0)
            return (int)index;
    }
    return -1;
}

static int safe_component(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;

    if (*cursor == '\0' || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    while (*cursor != '\0') {
        if (*cursor < 0x20u || *cursor == 0x7fu || *cursor == '/' ||
            *cursor == '\\' || *cursor == ':')
            return 0;
        ++cursor;
    }
    return 1;
}

static int append_component(char output[INSTALLER_PATH_SIZE],
                            const char *parent, const char *name)
{
    int length;

    if (strcmp(parent, "/") == 0)
        length = snprintf(output, INSTALLER_PATH_SIZE, "/%s", name);
    else
        length = snprintf(output, INSTALLER_PATH_SIZE, "%s/%s", parent,
                          name);
    return length >= 0 && length < INSTALLER_PATH_SIZE
               ? 0
               : -ENAMETOOLONG;
}

static int make_usb_path(const installer_result_t *result,
                         char output[INSTALLER_PATH_SIZE],
                         const char *partition, const char *relative_path)
{
    int length = snprintf(output, INSTALLER_PATH_SIZE, "%s/%s%s",
                          result->source_root, partition, relative_path);

    return length >= 0 && length < INSTALLER_PATH_SIZE
               ? 0
               : -ENAMETOOLONG;
}

static int make_destination_path(char output[INSTALLER_PATH_SIZE],
                         const installer_partition_result_t *partition,
                         const char *relative_path)
{
    int length = snprintf(output, INSTALLER_PATH_SIZE, "%s%s",
                          partition->root_prefix, relative_path);

    return length >= 0 && length < INSTALLER_PATH_SIZE
               ? 0
               : -ENAMETOOLONG;
}

static int append_child(child_name_t **children, size_t *count,
                        const char *name)
{
    child_name_t *resized =
        realloc(*children, (*count + 1u) * sizeof(**children));

    if (resized == NULL)
        return -ENOMEM;
    *children = resized;
    snprintf(resized[*count].name, sizeof(resized[*count].name), "%s", name);
    ++*count;
    return 0;
}

static int scan_directory(installer_result_t *result, unsigned int index,
                          const char *relative_path, int is_root)
{
    installer_partition_result_t *partition = &result->partitions[index];
    child_name_t *children = NULL;
    size_t child_count = 0;
    size_t child_index;
    char directory_path[INSTALLER_PATH_SIZE];
    iox_dirent_t entry;
    int directory_fd;
    int read_result = 0;
    int close_result;
    int return_value;

    return_value = make_usb_path(result, directory_path, partition->name,
                                 relative_path);
    if (return_value < 0) {
        set_failure(result, return_value, partition->name, relative_path,
                    "source_path_too_long");
        return return_value;
    }
    directory_fd = fileXioDopen(directory_path);
    if (is_root)
        partition->source_open_result = directory_fd;
    if (directory_fd < 0) {
        set_failure(result, directory_fd, partition->name, relative_path,
                    "source_dopen");
        return directory_fd;
    }

    for (;;) {
        char child_path[INSTALLER_PATH_SIZE];

        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(directory_fd, &entry);
        if (read_result <= 0)
            break;
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;
        if (!safe_component(entry.name)) {
            set_failure(result, -EINVAL, partition->name, relative_path,
                        "unsafe_source_component");
            break;
        }
        return_value = append_component(child_path, relative_path, entry.name);
        if (return_value < 0) {
            set_failure(result, return_value, partition->name, relative_path,
                        "source_path_too_long");
            break;
        }
        if (FIO_S_ISDIR(entry.stat.mode)) {
            return_value = append_item(result, index,
                                       INSTALLER_ITEM_DIRECTORY, child_path,
                                       0);
            if (return_value >= 0)
                return_value = append_child(&children, &child_count,
                                            entry.name);
            if (return_value < 0) {
                set_failure(result, return_value, partition->name, child_path,
                            "allocate_source_directory");
                break;
            }
            ++partition->source_directories;
            ++result->source_directory_count;
        } else {
            u64 size = ((u64)entry.stat.hisize << 32) | entry.stat.size;

            return_value = append_item(result, index, INSTALLER_ITEM_FILE,
                                       child_path, size);
            if (return_value < 0) {
                set_failure(result, return_value, partition->name, child_path,
                            "allocate_source_file");
                break;
            }
            ++partition->source_files;
            partition->source_bytes += size;
            ++result->source_file_count;
            result->source_total_bytes += size;
        }
    }

    if (is_root)
        partition->source_final_read_result = read_result;
    close_result = fileXioDclose(directory_fd);
    if (is_root)
        partition->source_close_result = close_result;
    if (result->failure_return == 0 && read_result < 0)
        set_failure(result, read_result, partition->name, relative_path,
                    "source_dread");
    if (result->failure_return == 0 && close_result < 0)
        set_failure(result, close_result, partition->name, relative_path,
                    "source_dclose");

    for (child_index = 0;
         child_index < child_count && result->failure_return == 0;
         ++child_index) {
        char child_path[INSTALLER_PATH_SIZE];

        return_value = append_component(child_path, relative_path,
                                        children[child_index].name);
        if (return_value < 0) {
            set_failure(result, return_value, partition->name, relative_path,
                        "source_path_too_long");
            break;
        }
        scan_directory(result, index, child_path, 0);
    }
    free(children);
    return result->failure_return;
}

void installer_scan_source(installer_result_t *result)
{
    iox_dirent_t entry;
    int root_fd;
    int read_result = 0;
    int close_result;

    result->source_scan_attempted = 1;
    root_fd = fileXioDopen(result->source_root);
    result->source_root_open_result = root_fd;
    if (root_fd < 0) {
        set_failure(result, root_fd, "", "/", "source_root_dopen");
        return;
    }

    for (;;) {
        int partition_index;
        int return_value;

        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(root_fd, &entry);
        if (read_result <= 0)
            break;
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;
        partition_index = partition_index_from_name(result, entry.name);
        if (partition_index < 0) {
            return_value = append_unknown(result, entry.name);
            if (return_value < 0) {
                set_failure(result, return_value, "", "/",
                            "allocate_unknown_top_level");
                break;
            }
            continue;
        }
        if (!FIO_S_ISDIR(entry.stat.mode) ||
            result->partitions[partition_index].source_present) {
            set_failure(result, -EINVAL,
                        result->partitions[partition_index].name, "/",
                        "invalid_partition_source_entry");
            break;
        }
        result->partitions[partition_index].source_present = 1;
        ++result->source_partition_count;
        ++result->partitions[partition_index].source_directories;
        ++result->source_directory_count;
    }
    result->source_root_final_read_result = read_result;
    close_result = fileXioDclose(root_fd);
    result->source_root_close_result = close_result;
    if (result->failure_return == 0 && read_result < 0)
        set_failure(result, read_result, "", "/", "source_root_dread");
    if (result->failure_return == 0 && close_result < 0)
        set_failure(result, close_result, "", "/", "source_root_dclose");

    if (result->failure_return == 0) {
        unsigned int index;

        for (index = 0; index < result->partition_count; ++index) {
            if (result->partitions[index].source_present)
                scan_directory(result, index, "/", 1);
            if (result->failure_return != 0)
                break;
        }
    }
    if (result->failure_return == 0 &&
        result->source_partition_count != result->partition_count)
        set_failure(result, -ENOENT, "", "/", "missing_partition_source");
    if (result->failure_return == 0 && result->source_file_count == 0)
        set_failure(result, -ENOENT, "", "/", "no_source_files");
    result->source_scan_valid = result->failure_return == 0;
}

static u32 copied_file_count(const installer_result_t *result)
{
    unsigned int index;
    u32 count = 0;

    for (index = 0; index < result->partition_count; ++index)
        count += result->partitions[index].copied_files;
    return count;
}

static u32 progress_elapsed_ms(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime() - progress_start_time,
                      &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static const char *progress_filename(const char *relative_path)
{
    const char *slash = strrchr(relative_path, '/');

    return slash != NULL && slash[1] != '\0' ? slash + 1 : relative_path;
}

static void format_file_size(char *text, size_t text_size, u64 bytes)
{
    const u64 kib = 1024u;
    const u64 mib = 1024u * 1024u;
    const u64 gib = 1024u * 1024u * 1024u;
    u64 unit;
    unsigned int tenth;
    const char *suffix;

    if (bytes < kib) {
        snprintf(text, text_size, "%llu B", (unsigned long long)bytes);
        return;
    }
    if (bytes < mib) {
        unit = kib;
        suffix = "KiB";
    } else if (bytes < gib) {
        unit = mib;
        suffix = "MiB";
    } else {
        unit = gib;
        suffix = "GiB";
    }
    tenth = (unsigned int)((bytes % unit) * 10u / unit);
    snprintf(text, text_size, "%llu.%u %s",
             (unsigned long long)(bytes / unit),
             tenth, suffix);
}

static void progress_line(int y, const char *text)
{
    ui_set_position(UI_SAFE_LEFT, y);
    ui_printf("%-*.*s", PROGRESS_LINE_WIDTH, PROGRESS_LINE_WIDTH, text);
}

static void show_progress(installer_result_t *result,
                          const installer_partition_result_t *partition,
                          const char *relative_path, const char *phase,
                          u64 file_processed)
{
    int verifying = strcmp(phase, "VERIFYING") == 0 ||
                    strcmp(phase, "VERIFIED") == 0;
    u64 total_work = result->source_total_bytes * 2u;
    u64 file_total_work = result->current_file_size * 2u;
    u64 file_work = (verifying ? result->current_file_size : 0u) +
                    file_processed;
    u64 overall_work = result->total_completed_file_bytes * 2u +
        (verifying ? result->current_file_size : 0u) + file_processed;
    u64 file_percent = file_total_work == 0
                           ? 100u
                           : file_work * 100u / file_total_work;
    u64 overall_percent = total_work == 0
                              ? 100u
                              : overall_work * 100u / total_work;
    u32 elapsed = progress_elapsed_ms();
    char line[128];
    char size_text[32];
    char step_line[128];

    if (overall_work > total_work)
        overall_work = total_work;
    if (overall_percent > 100u)
        overall_percent = 100u;
    if (progress_screen_ready && overall_percent < 100u &&
        elapsed - progress_last_draw_ms < PROGRESS_REFRESH_MS)
        return;

    if (!progress_screen_ready) {
        ui_begin();
        ui_printf("RepairBox.pl PSX HDD Setup v1.0\n");
        ui_inverse_status("INSTALLING SYSTEM");
        ui_inverse_status("DO NOT POWER OFF");
        progress_screen_ready = 1;
    }
    snprintf(step_line, sizeof(step_line), "Step %u / %u   %s",
             result->progress_step, result->progress_step_count,
             result->progress_operation);
    progress_line(72, step_line);
    snprintf(line, sizeof(line), "INSTALLING  %s", partition->name);
    progress_line(96, line);
    snprintf(line, sizeof(line), "File %u / %u   Name: %s",
             result->current_file_index, result->source_file_count,
             progress_filename(relative_path));
    progress_line(120, line);
    format_file_size(size_text, sizeof(size_text), result->current_file_size);
    snprintf(line, sizeof(line), "Current file: %llu%%   Size: %s",
             file_percent > 100u ? 100u : file_percent, size_text);
    progress_line(144, line);
    snprintf(line, sizeof(line), "Overall: %llu%%   Elapsed: %02u:%02u",
             overall_percent, elapsed / 60000u,
             (elapsed / 1000u) % 60u);
    progress_line(168, line);
    ui_sync();
    progress_last_draw_ms = elapsed;
}

static int hash_open_file(const char *path, u64 *size,
                           unsigned char digest[SHA256_DIGEST_SIZE],
                           installer_result_t *result,
                           installer_partition_result_t *partition,
                           const installer_source_item_t *item)
{
    sha256_context_t hash;
    int fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    u64 next_update = 0;
    u64 verify_start = GetTimerSystemTime();

    if (fd < 0)
        return fd;
    *size = 0;
    sha256_init(&hash);
    for (;;) {
        int read_result = fileXioRead(fd, copy_buffer, sizeof(copy_buffer));

        if (read_result < 0) {
            fileXioClose(fd);
            return read_result;
        }
        if (read_result == 0)
            break;
        sha256_update(&hash, copy_buffer, (size_t)read_result);
        *size += (u64)read_result;
        result->current_file_bytes_processed = *size;
        if (*size >= next_update) {
            show_progress(result, partition, item->relative_path,
                          "VERIFYING", *size);
            next_update = *size + 1048576u;
        }
    }
    {
        int close_result = fileXioClose(fd);
        if (close_result < 0)
            return close_result;
    }
    sha256_final(&hash, digest);
    {
        u32 seconds;
        u32 microseconds;

        TimerBusClock2USec(GetTimerSystemTime() - verify_start,
                          &seconds, &microseconds);
        result->verify_duration_ms +=
            seconds * 1000u + microseconds / 1000u;
    }
    return 0;
}

static int ensure_destination_directory(installer_result_t *result,
                                        installer_partition_result_t *partition,
                                        const char *relative_path)
{
    char destination[INSTALLER_PATH_SIZE];
    int return_value = make_destination_path(destination, partition,
                                             relative_path);

    if (return_value < 0)
        return return_value;
    return_value = fileXioMkdir(destination, 0777);
    if (return_value >= 0) {
        ++partition->created_directories;
        return 0;
    }
    {
        int directory_fd = fileXioDopen(destination);

        if (directory_fd < 0)
            return return_value;
        return_value = fileXioDclose(directory_fd);
        if (return_value < 0)
            return return_value;
    }
    (void)result;
    return 0;
}

static int copy_file(installer_result_t *result,
                     installer_partition_result_t *partition,
                     const installer_source_item_t *item)
{
    char source[INSTALLER_PATH_SIZE];
    char destination[INSTALLER_PATH_SIZE];
    sha256_context_t source_hash;
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    unsigned char destination_digest[SHA256_DIGEST_SIZE];
    u64 source_size = 0;
    u64 destination_size = 0;
    int source_fd = -1;
    int destination_fd = -1;
    int return_value;
    u64 next_update = 0;

    return_value = make_usb_path(result, source, partition->name,
                                 item->relative_path);
    if (return_value < 0)
        goto failed;
    return_value = make_destination_path(destination, partition,
                                         item->relative_path);
    if (return_value < 0)
        goto failed;

    result->current_file_size = item->size;
    result->current_file_bytes_processed = 0;
    result->current_file_index = copied_file_count(result) + 1u;
    show_progress(result, partition, item->relative_path, "COPYING", 0);
    source_fd = fileXioOpen(source, FIO_O_RDONLY, 0);
    if (source_fd < 0) {
        return_value = source_fd;
        goto failed;
    }
    destination_fd = fileXioOpen(destination,
                                 FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC,
                                 0666);
    if (destination_fd < 0) {
        return_value = destination_fd;
        goto failed;
    }
    sha256_init(&source_hash);
    for (;;) {
        int read_result = fileXioRead(source_fd, copy_buffer,
                                     sizeof(copy_buffer));
        int written = 0;

        if (read_result < 0) {
            return_value = read_result;
            goto failed;
        }
        if (read_result == 0)
            break;
        sha256_update(&source_hash, copy_buffer, (size_t)read_result);
        source_size += (u64)read_result;
        while (written < read_result) {
            int write_result = fileXioWrite(destination_fd,
                                            copy_buffer + written,
                                            read_result - written);

            if (write_result <= 0) {
                return_value = write_result < 0 ? write_result : -EIO;
                goto failed;
            }
            written += write_result;
        }
        result->current_file_bytes_processed = source_size;
        if (source_size >= next_update) {
            show_progress(result, partition, item->relative_path,
                          "COPYING", source_size);
            next_update = source_size + 1048576u;
        }
    }
    return_value = fileXioClose(source_fd);
    source_fd = -1;
    if (return_value < 0)
        goto failed;
    return_value = fileXioClose(destination_fd);
    destination_fd = -1;
    if (return_value < 0)
        goto failed;
    sha256_final(&source_hash, source_digest);
    ++partition->copied_files;
    partition->copied_bytes += source_size;

    if (source_size != item->size) {
        return_value = -EIO;
        goto failed;
    }
    show_progress(result, partition, item->relative_path,
                  "VERIFYING", 0);
    return_value = hash_open_file(destination, &destination_size,
                                  destination_digest, result, partition,
                                  item);
    if (return_value < 0)
        goto failed;
    if (source_size != destination_size ||
        memcmp(source_digest, destination_digest, SHA256_DIGEST_SIZE) != 0) {
        return_value = -EIO;
        goto failed;
    }
    ++partition->verified_files;
    show_progress(result, partition, item->relative_path,
                  "VERIFIED", source_size);
    result->total_completed_file_bytes += source_size;
    return 0;

failed:
    if (source_fd >= 0)
        fileXioClose(source_fd);
    if (destination_fd >= 0)
        fileXioClose(destination_fd);
    set_failure(result, return_value, partition->name, item->relative_path,
                "copy_or_verify_file");
    return return_value;
}

static int install_partition(installer_result_t *result,
                             unsigned int partition_index)
{
    installer_partition_result_t *partition =
        &result->partitions[partition_index];
    size_t item_index;

    partition->mount_attempted = 1;
    partition->mount_result =
        fileXioMount(partition->mountpoint, partition->blockdev, FIO_MT_RDWR);
    if (partition->mount_result >= 0) {
        partition->mount_owned = 1;
    } else if (partition->allow_preexisting_mount &&
               partition->mount_result == -EBUSY) {
        char root_path[32];

        partition->mount_preexisting = 1;
        snprintf(root_path, sizeof(root_path), "%s/", partition->root_prefix);
        partition->mount_probe_result = fileXioDopen(root_path);
        if (partition->mount_probe_result >= 0)
            partition->mount_probe_close_result =
                fileXioDclose(partition->mount_probe_result);
        if (partition->mount_probe_result < 0 ||
            partition->mount_probe_close_result < 0) {
            int probe_error = partition->mount_probe_result < 0
                                  ? partition->mount_probe_result
                                  : partition->mount_probe_close_result;

            set_failure(result, probe_error, partition->name, "/",
                        "probe_preexisting_mount");
            return probe_error;
        }
    } else {
        set_failure(result, partition->mount_result, partition->name, "/",
                    "mount_rw");
        return partition->mount_result;
    }

    for (item_index = 0;
         item_index < result->item_count && result->failure_return == 0;
         ++item_index) {
        const installer_source_item_t *item = &result->items[item_index];
        int return_value;

        if (item->partition_index != partition_index)
            continue;
        if (item->type == INSTALLER_ITEM_DIRECTORY) {
            return_value = ensure_destination_directory(
                result, partition, item->relative_path);
            if (return_value < 0)
                set_failure(result, return_value, partition->name,
                            item->relative_path,
                            "create_destination_directory");
        } else {
            copy_file(result, partition, item);
        }
    }

    if (partition->mount_owned) {
        partition->umount_attempted = 1;
        partition->umount_result = fileXioUmount(partition->mountpoint);
        if (partition->umount_result < 0) {
            result->all_unmounted_cleanly = 0;
            set_failure(result, partition->umount_result, partition->name,
                        "/", "umount");
        }
    }
    return result->failure_return;
}

void installer_execute(installer_result_t *result)
{
    unsigned int index;
    u32 verified_total = 0;

    if (result->preflight_failure_mask != 0 || !result->source_scan_valid ||
        !result->confirmation_received || result->session_write_locked)
        return;
    result->session_write_locked = 1;
    result->install_attempted = 1;
    result->all_unmounted_cleanly = 1;
    progress_start_time = GetTimerSystemTime();
    progress_last_draw_ms = 0;
    progress_screen_ready = 0;

    for (index = 0;
         index < result->partition_count && result->failure_return == 0;
         ++index) {
        if (result->partitions[index].source_files != 0)
            install_partition(result, index);
    }
    for (index = 0; index < result->partition_count; ++index)
        verified_total += result->partitions[index].verified_files;
    result->system_files_install_valid =
        result->failure_return == 0 && result->all_unmounted_cleanly &&
        verified_total == result->source_file_count;
    result->copy_elapsed_ms = progress_elapsed_ms();
}
