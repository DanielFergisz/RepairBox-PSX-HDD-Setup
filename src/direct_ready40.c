#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <fileXio_rpc.h>
#include <hdd-ioctl.h>

#include "direct_ready40.h"

static const u32 normal_zone = 8192u;

static const struct {
    const char *name;
    const char *blockdev;
} normal_targets[DR40_NORMAL_PFS_COUNT] = {
    {"__net", "hdd0:__net"},
    {"__system", "hdd0:__system"},
    {"__sysconf", "hdd0:__sysconf"},
    {"__common", "hdd0:__common"},
};

static const struct {
    const char *name;
    const char *blockdev;
    const char *mountpoint;
    const char *root_path;
    u32 zone;
} dvr_targets[DR40_DVR_PFS_COUNT] = {
    {"__xdata", "dvr_hdd0:__xdata", "dvr_pfs1:", "dvr_pfs1:/", 8192u},
    {"__xcontents", "dvr_hdd0:__xcontents", "dvr_pfs0:",
     "dvr_pfs0:/", 0x00100000u},
};

static u32 elapsed_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static dr40_snapshot_t take_snapshot(const storage_diagnostics_t *diagnostics)
{
    dr40_snapshot_t result;

    memset(&result, 0, sizeof(result));
    result.hdd_status = diagnostics->hdd.status_result;
    result.hdd_formatver = diagnostics->hdd.format_version_result;
    result.hdd_totalsector = diagnostics->hdd.total_sectors_result;
    result.hdd_maxsector = diagnostics->hdd.max_partition_sectors_result;
    result.hdd_available = diagnostics->hdd.available;
    result.hdd_entries = diagnostics->hdd.entry_count;
    result.dvr_status = diagnostics->dvr_hdd.status_result;
    result.dvr_formatver = diagnostics->dvr_hdd.format_version_result;
    result.dvr_totalsector = diagnostics->dvr_hdd.total_sectors_result;
    result.dvr_maxsector = diagnostics->dvr_hdd.max_partition_sectors_result;
    result.dvr_available = diagnostics->dvr_hdd.available;
    result.dvr_entries = diagnostics->dvr_hdd.entry_count;
    result.getmaxlba48 = diagnostics->dvr_hdd.max_lba48_result;
    result.islba48 = diagnostics->dvr_hdd.is_lba48_result;
    return result;
}

static const storage_entry_result_t *find_entry(
    const storage_device_result_t *device, const char *name)
{
    unsigned int index;

    for (index = 0; index < device->entry_count; ++index) {
        if (strcmp(device->entries[index].name, name) == 0)
            return &device->entries[index];
    }
    return NULL;
}

static int entry_matches(const storage_device_result_t *device,
                         const char *name, u32 start, u32 size)
{
    const storage_entry_result_t *entry = find_entry(device, name);

    return entry != NULL && entry->private_fields[5] == start &&
           entry->hisize == 0 && entry->size == size;
}

int dr40_exact_system_layout(const storage_device_result_t *device)
{
    return device->entry_count == DR40_SYSTEM_COUNT &&
        entry_matches(device, "__mbr", 0x00000000u, 0x00040000u) &&
        entry_matches(device, "__net", 0x00040000u, 0x00040000u) &&
        entry_matches(device, "__system", 0x00080000u, 0x00080000u) &&
        entry_matches(device, "__sysconf", 0x00100000u, 0x00100000u) &&
        entry_matches(device, "__common", 0x00200000u, 0x00200000u);
}

int dr40_exact_dvr_layout(const storage_device_result_t *device,
                          u32 *end, u32 *tail)
{
    const storage_entry_result_t *xcontents =
        find_entry(device, "__xcontents");
    u64 calculated_end = 0;

    if (xcontents != NULL)
        calculated_end = (u64)xcontents->private_fields[5] +
                         ((u64)xcontents->hisize << 32) + xcontents->size;
    if (end != NULL)
        *end = calculated_end <= 0xffffffffu ? (u32)calculated_end : 0;
    if (tail != NULL)
        *tail = calculated_end <= DR40_NATIVE_MAX
                    ? DR40_NATIVE_MAX - (u32)calculated_end : 0;
    return device->entry_count == DR40_DVR_COUNT &&
        entry_matches(device, "__extend", 0x04A817C8u, 0x00040000u) &&
        entry_matches(device, "__xdata", 0x04AC17C8u, 0x00200000u) &&
        entry_matches(device, "__xcontents", 0x04CC17C8u, 0x190BE038u) &&
        calculated_end == 0x1DD7F800u &&
        DR40_NATIVE_MAX - (u32)calculated_end == 0x800u;
}

static int stack_ready(const storage_diagnostics_t *diagnostics)
{
    unsigned int index;

    if (diagnostics->filexio_init_result < 0 ||
        diagnostics->cdvd_init_result <= 0 ||
        diagnostics->notice_game_start_result <= 0 ||
        diagnostics->xfrom_init_result < 0)
        return 0;
    for (index = 0; index < STORAGE_MODULE_COUNT; ++index) {
        if (diagnostics->modules[index].module_id < 0 ||
            diagnostics->modules[index].module_result < 0)
            return 0;
    }
    return 1;
}

static int hardware_profile(const dr40_snapshot_t *snapshot)
{
    return (u32)snapshot->getmaxlba48 == DR40_NATIVE_MAX &&
           snapshot->islba48 == 1 &&
           snapshot->hdd_totalsector >= 0 &&
           snapshot->dvr_totalsector >= 0 &&
           snapshot->hdd_totalsector == snapshot->dvr_totalsector;
}

static void initialize_operation(dr40_operation_t *operation,
                                 const char *name)
{
    memset(operation, 0, sizeof(*operation));
    operation->name = name;
    operation->return_value = -1;
}

static void initialize_pfs(dr40_pfs_result_t *pfs, const char *name,
                           const char *format_device, const char *blockdev,
                           const char *mountpoint, const char *root_path,
                           u32 zone)
{
    memset(pfs, 0, sizeof(*pfs));
    pfs->name = name;
    pfs->format_device = format_device;
    pfs->blockdev = blockdev;
    pfs->mountpoint = mountpoint;
    pfs->root_path = root_path;
    pfs->zone = zone;
    initialize_operation(&pfs->format, name);
    pfs->mount_result = -1;
    pfs->zone_size_result = -1;
    pfs->zone_free_result = -1;
    pfs->dopen_result = -1;
    pfs->final_dread_result = -1;
    pfs->dclose_result = -1;
    pfs->umount_attempted = 0;
    pfs->umount_result = -1;
}

void dr40_initialize(dr40_result_t *result,
                     const storage_diagnostics_t *diagnostics)
{
    unsigned int index;

    memset(result, 0, sizeof(*result));
    result->failed_phase = -1;
    result->failure_return = 0;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
    result->dvr_pfs0_cleanup_return = -1;
    result->dvr_pfs1_cleanup_return = -1;
    result->before = take_snapshot(diagnostics);
    result->after = result->before;
    initialize_operation(&result->setmax40, "SETMAX_40GB");
    initialize_operation(&result->apa, "FORMAT_APA");
    initialize_operation(&result->dvr_hdd, "FORMAT_DVR_HDD");
    for (index = 0; index < DR40_NORMAL_PFS_COUNT; ++index) {
        initialize_pfs(&result->normal_pfs[index], normal_targets[index].name,
                       "pfs:", normal_targets[index].blockdev,
                       "pfs0:", "pfs0:/", normal_zone);
    }
    for (index = 0; index < DR40_DVR_PFS_COUNT; ++index) {
        initialize_pfs(&result->dvr_pfs[index], dvr_targets[index].name,
                       "dvr_pfs:", dvr_targets[index].blockdev,
                       dvr_targets[index].mountpoint,
                       dvr_targets[index].root_path,
                       dvr_targets[index].zone);
    }
    bootstrap_initialize(&result->bootstrap);
    installer_initialize(&result->installer, diagnostics);
}

static void fail(dr40_result_t *result, int phase, const char *partition,
                 const char *path, const char *operation, int return_value)
{
    if (result->stopped_on_error)
        return;
    result->stopped_on_error = 1;
    result->failed_phase = phase;
    result->failure_return = return_value != 0 ? return_value : -EIO;
    snprintf(result->failure_partition, sizeof(result->failure_partition),
             "%s", partition != NULL ? partition : "");
    snprintf(result->failure_path, sizeof(result->failure_path), "%s",
             path != NULL ? path : "");
    snprintf(result->failure_operation, sizeof(result->failure_operation),
             "%s", operation != NULL ? operation : "unknown");
}

void dr40_scan_preflight(dr40_result_t *result,
                         storage_diagnostics_t *diagnostics)
{
    result->source_ready = 0;
    result->bootstrap_ready = 0;
    result->preflight_valid = 0;
    result->before = take_snapshot(diagnostics);
    result->stack_ready = stack_ready(diagnostics);
    result->hardware_profile_valid = hardware_profile(&result->before);
    result->visible_boundary_valid =
        (u32)result->before.hdd_totalsector == DR40_VISIBLE_SECTORS &&
        (u32)result->before.dvr_totalsector == DR40_VISIBLE_SECTORS;
    bootflag_ro_inspect(&result->bootflag);
    result->bootflag_normal =
        result->bootflag.state == BOOTFLAG_RO_NORMAL;
    if (!result->stack_ready || !result->hardware_profile_valid ||
        !result->bootflag_normal) {
        result->mode = DR40_MODE_STOP;
        result->preflight_valid = 0;
        return;
    }
    if (!result->visible_boundary_valid) {
        result->mode = DR40_MODE_SET_BOUNDARY;
        result->preflight_valid = 1;
        return;
    }
    result->mode = DR40_MODE_INITIALIZE;
    installer_release(&result->installer);
    installer_initialize(&result->installer, diagnostics);
    bootstrap_initialize(&result->bootstrap);
    installer_scan_source(&result->installer);
    result->source_ready = result->installer.source_scan_valid;
    bootstrap_validate_usb(&result->bootstrap);
    result->bootstrap_ready = result->bootstrap.validation_valid;
    if (result->installer.source_root_open_result >= 0) {
        result->usb_ready = 1;
        if (!result->usb_wait_attempted) {
            result->usb_root_last_dopen =
                result->installer.source_root_open_result;
            result->usb_root_close_result =
                result->installer.source_root_close_result;
        }
    } else if (result->bootstrap.binary_open_result >= 0) {
        result->usb_ready = 1;
    }
    result->preflight_valid = result->source_ready && result->bootstrap_ready;
}

static void emit(dr40_progress_callback_t progress, unsigned int stage,
                 const char *label, dr40_progress_phase_t phase,
                 int return_value, u32 duration_ms, void *context)
{
    if (progress != NULL)
        progress(stage, label, phase, return_value, duration_ms, context);
}

static int validate_pfs_read_only(dr40_pfs_result_t *pfs)
{
    iox_dirent_t entry;
    int read_result = -1;
    int allow_preexisting = strcmp(pfs->format_device, "dvr_pfs:") == 0;

    pfs->mount_preexisting = 0;
    pfs->mount_owned = 0;
    pfs->umount_attempted = 0;
    pfs->umount_result = -1;
    pfs->zone_size_result = -1;
    pfs->zone_free_result = -1;
    pfs->dopen_result = -1;
    pfs->final_dread_result = -1;
    pfs->dclose_result = -1;

    pfs->mount_result =
        fileXioMount(pfs->mountpoint, pfs->blockdev, FIO_MT_RDONLY);
    if (pfs->mount_result >= 0) {
        pfs->mount_owned = 1;
    } else if (allow_preexisting && pfs->mount_result == -EBUSY) {
        pfs->mount_preexisting = 1;
    } else {
        return 0;
    }
    pfs->zone_size_result = fileXioDevctl(
        pfs->mountpoint, PDIOC_ZONESZ, NULL, 0, NULL, 0);
    pfs->zone_free_result = fileXioDevctl(
        pfs->mountpoint, PDIOC_ZONEFREE, NULL, 0, NULL, 0);
    pfs->dopen_result = fileXioDopen(pfs->root_path);
    if (pfs->dopen_result >= 0) {
        do {
            memset(&entry, 0, sizeof(entry));
            read_result = fileXioDread(pfs->dopen_result, &entry);
        } while (read_result > 0);
        pfs->final_dread_result = read_result;
        pfs->dclose_result = fileXioDclose(pfs->dopen_result);
    }
    if (pfs->mount_owned) {
        pfs->umount_attempted = 1;
        pfs->umount_result = fileXioUmount(pfs->mountpoint);
    }
    pfs->filesystem_valid =
        (pfs->mount_owned || pfs->mount_preexisting) &&
        pfs->zone_size_result == (int)pfs->zone &&
        pfs->zone_free_result >= 0 && pfs->dopen_result >= 0 &&
        pfs->final_dread_result == 0 && pfs->dclose_result >= 0 &&
        (!pfs->mount_owned || pfs->umount_result >= 0);
    return pfs->filesystem_valid;
}

static int pfs_failure_return(const dr40_pfs_result_t *pfs)
{
    if (pfs->format.return_value < 0)
        return pfs->format.return_value;
    if (pfs->mount_result < 0 &&
        !(strcmp(pfs->format_device, "dvr_pfs:") == 0 &&
          pfs->mount_result == -EBUSY))
        return pfs->mount_result;
    if (pfs->zone_size_result < 0)
        return pfs->zone_size_result;
    if (pfs->zone_size_result != (int)pfs->zone)
        return -EIO;
    if (pfs->zone_free_result < 0)
        return pfs->zone_free_result;
    if (pfs->dopen_result < 0)
        return pfs->dopen_result;
    if (pfs->final_dread_result < 0)
        return pfs->final_dread_result;
    if (pfs->dclose_result < 0)
        return pfs->dclose_result;
    if (pfs->umount_attempted && pfs->umount_result < 0)
        return pfs->umount_result;
    return -EIO;
}

static int format_and_validate_pfs(dr40_pfs_result_t *pfs,
                                   unsigned int stage,
                                   dr40_progress_callback_t progress,
                                   void *context)
{
    u64 start;
    u64 end;

    emit(progress, stage, pfs->name, DR40_PROGRESS_BEGIN, -1, 0, context);
    pfs->format.attempted = 1;
    start = GetTimerSystemTime();
    pfs->format.return_value = fileXioFormat(
        pfs->format_device, pfs->blockdev,
        (const char *)&pfs->zone, sizeof(pfs->zone));
    end = GetTimerSystemTime();
    pfs->format.duration_ms = elapsed_ms(start, end);
    if (pfs->format.return_value < 0) {
        emit(progress, stage, pfs->name, DR40_PROGRESS_FAILED,
             pfs->format.return_value, pfs->format.duration_ms, context);
        return 0;
    }
    emit(progress, stage, pfs->name, DR40_PROGRESS_VALIDATE,
         pfs->format.return_value, pfs->format.duration_ms, context);
    pfs->format.valid = validate_pfs_read_only(pfs);
    emit(progress, stage, pfs->name,
         pfs->format.valid ? DR40_PROGRESS_COMPLETE : DR40_PROGRESS_FAILED,
         pfs->format.return_value, pfs->format.duration_ms, context);
    return pfs->format.valid;
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
        return fileXioDclose(fd) < 0 ? -EIO : 0;
    }
}

static int ensure_xcontents_directories(dr40_result_t *result)
{
    static const char *const paths[] = {
        "dvr_pfs0:/dvr", "dvr_pfs0:/result", "dvr_pfs0:/epg",
    };
    unsigned int index;
    int mount_result = fileXioMount(
        "dvr_pfs0:", "dvr_hdd0:__xcontents", FIO_MT_RDWR);
    int return_value = mount_result;
    int mount_owned = mount_result >= 0;

    if (mount_result < 0 && mount_result != -EBUSY)
        return mount_result;
    return_value = 0;
    for (index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        return_value = ensure_directory(paths[index]);
        if (return_value < 0)
            break;
    }
    if (mount_owned) {
        int umount_result = fileXioUmount("dvr_pfs0:");

        if (return_value >= 0 && umount_result < 0)
            return_value = umount_result;
    }
    result->dvr_directories_valid = return_value >= 0;
    return return_value;
}

static int validate_xcontents_directories(void)
{
    static const char *const paths[] = {
        "dvr_pfs0:/dvr", "dvr_pfs0:/result", "dvr_pfs0:/epg",
    };
    unsigned int index;
    int result = fileXioMount(
        "dvr_pfs0:", "dvr_hdd0:__xcontents", FIO_MT_RDONLY);
    int mount_owned = result >= 0;

    if (result < 0 && result != -EBUSY)
        return 0;
    result = 0;
    for (index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        int fd = fileXioDopen(paths[index]);

        if (fd < 0) {
            result = fd;
            break;
        }
        result = fileXioDclose(fd);
        if (result < 0)
            break;
    }
    if (mount_owned && fileXioUmount("dvr_pfs0:") < 0)
        result = -EIO;
    return result >= 0;
}

static void refresh_after(storage_diagnostics_t *diagnostics,
                           dr40_result_t *result)
{
    storage_refresh_layout_diagnostics(diagnostics);
    result->after = take_snapshot(diagnostics);
}

static void cleanup_stale_dvr_mounts(dr40_result_t *result)
{
    /*
     * dvrfile can preserve the Sony DVR automounts from the layout visible
     * when the IOP stack was started. A PC format only clears the beginning
     * of the disk, so dvr_pfs0:/dvr_pfs1: may still refer to the old table.
     * Release them before rebuilding either APA namespace. An error is not
     * fatal here because an unmounted clean device also returns an error;
     * the normal post-format mount/readback remains the authoritative gate.
     */
    result->dvr_mount_cleanup_attempted = 1;
    result->dvr_pfs0_cleanup_return = fileXioUmount("dvr_pfs0:");
    result->dvr_pfs1_cleanup_return = fileXioUmount("dvr_pfs1:");
}

static void execute_setmax(dr40_result_t *result,
                           storage_diagnostics_t *diagnostics,
                           dr40_progress_callback_t progress, void *context)
{
    u32 value = DR40_VISIBLE_SECTORS;
    u64 start;
    u64 end;

    result->session_write_locked = 1;
    emit(progress, 1, "SET 40 GB BOUNDARY", DR40_PROGRESS_BEGIN,
         -1, 0, context);
    result->setmax40.attempted = 1;
    start = GetTimerSystemTime();
    result->setmax40.return_value = fileXioDevctl(
        "dvr_hdd0:", HDIOC_SETMAXLBA28,
        &value, sizeof(value), NULL, 0);
    end = GetTimerSystemTime();
    result->setmax40.duration_ms = elapsed_ms(start, end);
    result->setmax40.valid = result->setmax40.return_value >= 0;
    result->power_cycle_required = 1;
    emit(progress, 1, "SET 40 GB BOUNDARY",
         result->setmax40.valid ? DR40_PROGRESS_COMPLETE
                                : DR40_PROGRESS_FAILED,
         result->setmax40.return_value, result->setmax40.duration_ms,
         context);
    if (!result->setmax40.valid)
        fail(result, 0, "", "", "HDIOC_SETMAXLBA28",
             result->setmax40.return_value);
    refresh_after(diagnostics, result);
}

void dr40_execute(dr40_result_t *result,
                  storage_diagnostics_t *diagnostics,
                  dr40_progress_callback_t progress, void *context)
{
    unsigned int index;
    unsigned int stage;
    u64 start;
    u64 end;

    if (!result->confirmation_received || result->session_write_locked ||
        !result->preflight_valid)
        return;
    if (result->mode == DR40_MODE_SET_BOUNDARY) {
        execute_setmax(result, diagnostics, progress, context);
        return;
    }
    if (result->mode != DR40_MODE_INITIALIZE)
        return;
    result->session_write_locked = 1;
    cleanup_stale_dvr_mounts(result);

    emit(progress, 1, "FORMAT SYSTEM APA", DR40_PROGRESS_BEGIN,
         -1, 0, context);
    result->apa.attempted = 1;
    start = GetTimerSystemTime();
    result->apa.return_value = fileXioFormat("hdd0:", NULL, NULL, 0);
    end = GetTimerSystemTime();
    result->apa.duration_ms = elapsed_ms(start, end);
    if (result->apa.return_value < 0) {
        emit(progress, 1, "FORMAT SYSTEM APA", DR40_PROGRESS_FAILED,
             result->apa.return_value, result->apa.duration_ms, context);
        fail(result, 1, "__mbr", "/", "fileXioFormat_hdd0",
             result->apa.return_value);
        goto finished;
    }
    refresh_after(diagnostics, result);
    result->system_layout_valid =
        result->after.hdd_status == 0 &&
        result->after.hdd_formatver == 2 &&
        (u32)result->after.hdd_totalsector == DR40_VISIBLE_SECTORS &&
        dr40_exact_system_layout(&diagnostics->hdd);
    result->apa.valid = result->system_layout_valid;
    emit(progress, 1, "FORMAT SYSTEM APA",
         result->apa.valid ? DR40_PROGRESS_COMPLETE : DR40_PROGRESS_FAILED,
         result->apa.return_value, result->apa.duration_ms, context);
    if (!result->apa.valid) {
        fail(result, 1, "__mbr", "/", "validate_system_layout", -EIO);
        goto finished;
    }

    for (index = 0; index < DR40_NORMAL_PFS_COUNT; ++index) {
        stage = index + 2u;
        if (!format_and_validate_pfs(&result->normal_pfs[index], stage,
                                     progress, context)) {
            fail(result, (int)stage, result->normal_pfs[index].name, "/",
                  "format_or_validate_normal_pfs",
                  pfs_failure_return(&result->normal_pfs[index]));
            goto finished;
        }
    }

    stage = 6;
    emit(progress, stage, "FORMAT FINAL DVR HDD", DR40_PROGRESS_BEGIN,
         -1, 0, context);
    result->dvr_hdd.attempted = 1;
    start = GetTimerSystemTime();
    result->dvr_hdd.return_value =
        fileXioFormat("dvr_hdd0:", NULL, NULL, 0);
    end = GetTimerSystemTime();
    result->dvr_hdd.duration_ms = elapsed_ms(start, end);
    if (result->dvr_hdd.return_value < 0) {
        emit(progress, stage, "FORMAT FINAL DVR HDD", DR40_PROGRESS_FAILED,
             result->dvr_hdd.return_value, result->dvr_hdd.duration_ms,
             context);
        fail(result, 6, "dvr_hdd0", "/", "fileXioFormat_dvr_hdd0",
             result->dvr_hdd.return_value);
        goto finished;
    }
    refresh_after(diagnostics, result);
    result->dvr_layout_valid = dr40_exact_dvr_layout(
        &diagnostics->dvr_hdd, &result->xcontents_end,
        &result->tail_reserved_sectors);
    result->dvr_hdd.valid = result->dvr_layout_valid;
    emit(progress, stage, "FORMAT FINAL DVR HDD",
         result->dvr_layout_valid ? DR40_PROGRESS_COMPLETE
                                  : DR40_PROGRESS_FAILED,
         result->dvr_hdd.return_value, result->dvr_hdd.duration_ms,
         context);
    if (!result->dvr_layout_valid) {
        fail(result, 6, "dvr_hdd0", "/", "validate_final_dvr_layout",
             -EIO);
        goto finished;
    }

    for (index = 0; index < DR40_DVR_PFS_COUNT; ++index) {
        stage = index + 7u;
        if (!format_and_validate_pfs(&result->dvr_pfs[index], stage,
                                     progress, context)) {
            fail(result, (int)stage, result->dvr_pfs[index].name, "/",
                  "format_or_validate_dvr_pfs",
                  pfs_failure_return(&result->dvr_pfs[index]));
            goto finished;
        }
    }

    stage = 9;
    emit(progress, stage, "CREATE DVR DIRECTORIES", DR40_PROGRESS_BEGIN,
         -1, 0, context);
    {
        int directory_result = ensure_xcontents_directories(result);

        emit(progress, stage, "CREATE DVR DIRECTORIES",
             directory_result >= 0 ? DR40_PROGRESS_COMPLETE
                                   : DR40_PROGRESS_FAILED,
             directory_result, 0, context);
        if (directory_result < 0) {
            fail(result, 9, "__xcontents", "/",
                 "create_dvr_result_epg", directory_result);
            goto finished;
        }
    }

    stage = 10;
    emit(progress, stage, "INSTALL MBR/OSD BOOTSTRAP",
         DR40_PROGRESS_BEGIN, -1, 0, context);
    {
        int bootstrap_return = bootstrap_write_and_verify(&result->bootstrap);

        emit(progress, stage, "INSTALL MBR/OSD BOOTSTRAP",
             bootstrap_return >= 0 ? DR40_PROGRESS_COMPLETE
                                   : DR40_PROGRESS_FAILED,
             bootstrap_return, result->bootstrap.write_duration_ms, context);
        if (bootstrap_return < 0) {
            result->failure_offset = result->bootstrap.failure_lba;
            result->failure_requested_bytes =
                result->bootstrap.failure_requested_bytes;
            result->failure_actual_bytes =
                result->bootstrap.failure_actual_bytes;
            fail(result, 10, "__mbr", BOOTSTRAP_BIN_PATH,
                 "write_or_verify_bootstrap", bootstrap_return);
            goto finished;
        }
    }

    stage = 11;
    emit(progress, stage, "COPY SYSTEM/DVR FILES",
         DR40_PROGRESS_BEGIN, -1, 0, context);
    result->installer.confirmation_received = 1;
    installer_execute(&result->installer);
    result->all_package_files_valid =
        result->installer.system_files_install_valid;
    emit(progress, stage, "COPY SYSTEM/DVR FILES",
         result->all_package_files_valid ? DR40_PROGRESS_COMPLETE
                                         : DR40_PROGRESS_FAILED,
         result->installer.failure_return, 0, context);
    if (!result->all_package_files_valid) {
        fail(result, 11, result->installer.failure_partition,
             result->installer.failure_path,
             result->installer.failure_operation,
             result->installer.failure_return);
        goto finished;
    }

    stage = 12;
    emit(progress, stage, "FINAL VALIDATION", DR40_PROGRESS_VALIDATE,
         0, 0, context);
    refresh_after(diagnostics, result);
    result->system_layout_valid = dr40_exact_system_layout(&diagnostics->hdd);
    result->dvr_layout_valid = dr40_exact_dvr_layout(
        &diagnostics->dvr_hdd, &result->xcontents_end,
        &result->tail_reserved_sectors);
    result->final_pfs_valid = 1;
    for (index = 0; index < DR40_NORMAL_PFS_COUNT; ++index) {
        if (!validate_pfs_read_only(&result->normal_pfs[index]))
            result->final_pfs_valid = 0;
    }
    for (index = 0; index < DR40_DVR_PFS_COUNT; ++index) {
        if (!validate_pfs_read_only(&result->dvr_pfs[index]))
            result->final_pfs_valid = 0;
    }
    if (!validate_xcontents_directories())
        result->final_pfs_valid = 0;
    bootflag_ro_inspect(&result->bootflag);
    result->bootflag_normal =
        result->bootflag.state == BOOTFLAG_RO_NORMAL;
    result->final_bootstrap_valid = result->bootstrap.bootstrap_valid;
    result->direct_ready40_storage_valid =
        result->after.hdd_status == 0 &&
        result->after.hdd_formatver == 2 &&
        (u32)result->after.hdd_totalsector == DR40_VISIBLE_SECTORS &&
        result->after.dvr_status == 0 &&
        result->after.dvr_formatver == 2 &&
        (u32)result->after.dvr_totalsector == DR40_VISIBLE_SECTORS &&
        (u32)result->after.getmaxlba48 == DR40_NATIVE_MAX &&
        result->after.islba48 == 1 && result->system_layout_valid &&
        result->dvr_layout_valid && result->final_pfs_valid &&
        result->all_package_files_valid &&
        result->final_bootstrap_valid && result->bootflag_normal;
    emit(progress, stage, "FINAL VALIDATION",
         result->direct_ready40_storage_valid ? DR40_PROGRESS_COMPLETE
                                              : DR40_PROGRESS_FAILED,
         result->direct_ready40_storage_valid ? 0 : -EIO, 0, context);
    if (!result->direct_ready40_storage_valid)
        fail(result, 12, "", "/", "final_validation", -EIO);

finished:
    refresh_after(diagnostics, result);
}

void dr40_release(dr40_result_t *result)
{
    installer_release(&result->installer);
}
