#include <delaythread.h>
#include <errno.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <hdd-ioctl.h>

#include "psx1_pipeline.h"
#include "build_profile.h"
#include "ui.h"

#define PSX1_STAGE_COUNT 8u
#define APA_HEADER_FIRST_LBA 0x00002000u
#define APA_HEADER_LBA_STEP 0x00040000u
#define APA_HEADER_SECTORS 2u
#define APA_HEADER_BYTES (APA_HEADER_SECTORS * 512u)
#define APA_POST_FORMAT_SCAN_LIMIT 4u
#define APA_POST_FORMAT_RETRY_US 500000u

static const int pfs_format_args[1] = {8192};
static unsigned char format_thread_stack[64 * 1024]
    __attribute__((aligned(64)));
static unsigned char recovery_transfer_buffer[sizeof(hddAtaTransfer_t) +
                                              APA_HEADER_BYTES]
    __attribute__((aligned(64)));
static unsigned char recovery_readback_buffer[APA_HEADER_BYTES]
    __attribute__((aligned(64)));

typedef enum format_job_kind {
    FORMAT_JOB_APA = 0,
    FORMAT_JOB_PFS = 1
} format_job_kind_t;

typedef struct format_job {
    format_job_kind_t kind;
    const char *blockdev;
    volatile int started;
    volatile int done;
    int result;
    u64 start_ticks;
    u64 end_ticks;
} format_job_t;

typedef struct operation_screen_state {
    unsigned int step;
    int ready;
    char operation[64];
} operation_screen_state_t;

static operation_screen_state_t operation_screen;

_Static_assert(sizeof(pfs_format_args) == 4,
               "PFS formatter requires one 32-bit zone-size argument");

static u32 elapsed_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static void reset_scan(inspector_data_t *scan)
{
    memset(scan, 0, sizeof(*scan));
    scan->hdd_status = -1;
    scan->hdd_formatver = -1;
    scan->hdd_totalsector = -1;
    scan->hdd_maxsector = -1;
    scan->hdd_getmaxlba48 = -1;
    scan->hdd_islba48 = -1;
    scan->mbr_read_result = -1;
}

static void draw_operation(unsigned int step, const char *phase,
                           const char *operation, const char *status,
                           u32 duration_ms)
{
    char line[96];
    int changed = !operation_screen.ready || operation_screen.step != step ||
                  strcmp(operation_screen.operation, operation) != 0;

    if (changed) {
        ui_begin();
        ui_printf(RBX_PROGRAM_TITLE "\n");
        ui_printf("PSX1 - First Revision\n\n");
        operation_screen.ready = 1;
        operation_screen.step = step;
        snprintf(operation_screen.operation,
                 sizeof(operation_screen.operation), "%s", operation);
    }
    ui_set_position(UI_SAFE_LEFT, 52);
    ui_inverse_status(phase);
    snprintf(line, sizeof(line), "Step %u / %u", step, PSX1_STAGE_COUNT);
    ui_set_position(UI_SAFE_LEFT, 76);
    ui_printf("%-54.54s", line);
    ui_set_position(UI_SAFE_LEFT, 100);
    ui_printf("%-54.54s", operation);
    ui_set_position(UI_SAFE_LEFT, 132);
    ui_inverse_status(status);
    if (duration_ms != 0) {
        snprintf(line, sizeof(line), "Elapsed %u.%03u s",
                 duration_ms / 1000u, duration_ms % 1000u);
        ui_set_position(UI_SAFE_LEFT, 156);
        ui_printf("%-54.54s", line);
    } else {
        ui_set_position(UI_SAFE_LEFT, 156);
        ui_printf("%-54.54s", "");
    }
    ui_sync();
}

static void format_worker(void *argument)
{
    format_job_t *job = (format_job_t *)argument;

    job->start_ticks = GetTimerSystemTime();
    job->started = 1;
    if (job->kind == FORMAT_JOB_APA)
        job->result = fileXioFormat("hdd0:", NULL, NULL, 0);
    else
        job->result = fileXioFormat("pfs:", job->blockdev,
                                    (const char *)&pfs_format_args,
                                    sizeof(pfs_format_args));
    job->end_ticks = GetTimerSystemTime();
    job->done = 1;
    ExitThread();
}

static int run_format_job(unsigned int step, const char *operation,
                          format_job_kind_t kind, const char *blockdev,
                          u32 *duration_ms)
{
    ee_thread_t thread;
    ee_thread_status_t status;
    format_job_t job;
    u64 ui_start = GetTimerSystemTime();
    int thread_id;
    int return_value;

    memset(&thread, 0, sizeof(thread));
    memset(&job, 0, sizeof(job));
    job.kind = kind;
    job.blockdev = blockdev;
    job.result = -1;
    draw_operation(step, "PREPARING STORAGE", operation,
                   "IN PROGRESS - DO NOT POWER OFF", 0);
    DelayThread(150000);
    thread.func = (void *)format_worker;
    thread.stack = format_thread_stack;
    thread.stack_size = sizeof(format_thread_stack);
    thread.gp_reg = &_gp;
    thread.initial_priority = 64;
    thread_id = CreateThread(&thread);
    if (thread_id < 0)
        return thread_id;
    return_value = StartThread(thread_id, &job);
    if (return_value < 0) {
        DeleteThread(thread_id);
        return return_value;
    }
    while (!job.done) {
        u64 start = job.started ? job.start_ticks : ui_start;
        draw_operation(step, "PREPARING STORAGE", operation,
                       "IN PROGRESS - DO NOT POWER OFF",
                       elapsed_ms(start, GetTimerSystemTime()));
        DelayThread(250000);
    }
    do {
        return_value = ReferThreadStatus(thread_id, &status);
        if (return_value < 0)
            return return_value;
        if (status.status != THS_DORMANT)
            DelayThread(1000);
    } while (status.status != THS_DORMANT);
    return_value = DeleteThread(thread_id);
    if (return_value < 0)
        return return_value;
    *duration_ms = elapsed_ms(job.start_ticks, job.end_ticks);
    draw_operation(step, "PREPARING STORAGE", operation,
                   job.result >= 0 ? "COMPLETE" : "FAILED", *duration_ms);
    DelayThread(250000);
    return job.result;
}

static const apa_entry_t *find_entry(const inspector_data_t *scan,
                                     const char *name)
{
    unsigned int index;

    for (index = 0; index < scan->apa_count; ++index) {
        if (strcmp(scan->apa[index].name, name) == 0)
            return &scan->apa[index];
    }
    return NULL;
}

static int validate_apa_after_format(psx1_result_t *result)
{
    unsigned int attempt;

    result->format.post_format_flush_result = 0;
    result->format.post_format_scan_attempts = 0;
    for (attempt = 0; attempt < APA_POST_FORMAT_SCAN_LIMIT; ++attempt) {
        reset_scan(&result->formatted);
        inspector_scan_layout(&result->formatted);
        ++result->format.post_format_scan_attempts;
        result->format.apa_layout_valid =
            format_test_exact_apa(&result->formatted);
        result->format.mbr_valid =
            format_test_mbr_valid(&result->formatted);
        if (result->format.apa_layout_valid && result->format.mbr_valid)
            return 1;
        if (attempt + 1u < APA_POST_FORMAT_SCAN_LIMIT) {
            result->format.post_format_flush_result =
                fileXioDevctl("hdd0:", HDIOC_FLUSH, NULL, 0, NULL, 0);
            DelayThread(APA_POST_FORMAT_RETRY_US);
        }
    }
    return 0;
}

static void draw_recovery_cleanup(const format_test_result_t *format,
                                  u32 current_lba, int *screen_ready)
{
    char line[96];

    if (!*screen_ready) {
        ui_begin();
        ui_printf(RBX_PROGRAM_TITLE "\n");
        ui_printf("PSX1 - First Revision\n\n");
        *screen_ready = 1;
    }
    ui_set_position(UI_SAFE_LEFT, 52);
    ui_inverse_status("APA RECOVERY CLEANUP");
    ui_set_position(UI_SAFE_LEFT, 76);
    ui_printf("%-54.54s", "Invalidating old APA header slots");
    snprintf(line, sizeof(line), "Slot %u / %u   LBA 0x%08X",
             format->recovery_cleanup_verified_slots,
             format->recovery_cleanup_slots, current_lba);
    ui_set_position(UI_SAFE_LEFT, 100);
    ui_printf("%-54.54s", line);
    ui_set_position(UI_SAFE_LEFT, 132);
    ui_inverse_status("IN PROGRESS - DO NOT POWER OFF");
    ui_sync();
}

static int cleanup_old_apa_headers(psx1_result_t *result)
{
    format_test_result_t *format = &result->format;
    u64 sector_count = format->physical_sector_count;
    u64 lba;
    u64 start = GetTimerSystemTime();
    int progress_screen_ready = 0;

    format->recovery_cleanup_attempted = 1;
    format->recovery_cleanup_result = -EIO;
    format->recovery_cleanup_failure_lba = 0;
    format->recovery_cleanup_slots = 0;
    format->recovery_cleanup_verified_slots = 0;
    if (sector_count > 0x100000000ULL)
        sector_count = 0x100000000ULL;
    if (sector_count <= APA_HEADER_FIRST_LBA + APA_HEADER_SECTORS) {
        format->recovery_cleanup_result = -EINVAL;
        format->recovery_cleanup_duration_ms =
            elapsed_ms(start, GetTimerSystemTime());
        return -EINVAL;
    }
    format->recovery_cleanup_slots = (u32)(
        (sector_count - APA_HEADER_FIRST_LBA - APA_HEADER_SECTORS) /
        APA_HEADER_LBA_STEP + 1u);
    memset(recovery_transfer_buffer, 0, sizeof(recovery_transfer_buffer));
    for (lba = APA_HEADER_FIRST_LBA;
         lba + APA_HEADER_SECTORS <= sector_count;
         lba += APA_HEADER_LBA_STEP) {
        hddAtaTransfer_t *transfer =
            (hddAtaTransfer_t *)recovery_transfer_buffer;
        hddAtaTransfer_t request;
        int io_result;

        if ((format->recovery_cleanup_verified_slots & 15u) == 0)
            draw_recovery_cleanup(format, (u32)lba,
                                  &progress_screen_ready);
        transfer->lba = (u32)lba;
        transfer->size = APA_HEADER_SECTORS;
        io_result = fileXioDevctl(
            "hdd0:", HDIOC_WRITESECTOR, transfer,
            sizeof(*transfer) + APA_HEADER_BYTES, NULL, 0);
        if (io_result < 0) {
            format->recovery_cleanup_failure_lba = (u32)lba;
            format->recovery_cleanup_result = io_result;
            goto finished;
        }
        request.lba = (u32)lba;
        request.size = APA_HEADER_SECTORS;
        memset(recovery_readback_buffer, 0xA5,
               sizeof(recovery_readback_buffer));
        io_result = fileXioDevctl(
            "hdd0:", HDIOC_READSECTOR, &request, sizeof(request),
            recovery_readback_buffer, sizeof(recovery_readback_buffer));
        if (io_result < 0 ||
            memcmp(transfer->data, recovery_readback_buffer,
                   APA_HEADER_BYTES) != 0) {
            format->recovery_cleanup_failure_lba = (u32)lba;
            format->recovery_cleanup_result =
                io_result < 0 ? io_result : -EIO;
            goto finished;
        }
        ++format->recovery_cleanup_verified_slots;
    }
    format->recovery_cleanup_result =
        fileXioDevctl("hdd0:", HDIOC_FLUSH, NULL, 0, NULL, 0);

finished:
    format->recovery_cleanup_duration_ms =
        elapsed_ms(start, GetTimerSystemTime());
    return format->recovery_cleanup_result;
}

static int installed_pfs_valid(const apa_entry_t *entry)
{
    const raw_pfs_diag_t *pfs = &entry->raw_pfs;

    return entry->pfs_applicable && pfs->applicable &&
           pfs->raw_pfs_read_valid && pfs->pfs_super_valid &&
           pfs->pfs_root_valid && pfs->pfs_structure_valid &&
           pfs->version == 3 && pfs->zone_size == 8192u &&
           pfs->declared_subpartitions == 0 &&
           pfs->available_subpartitions == 0 &&
           pfs->subpartition_class == PFS_SUBPART_MATCH &&
           pfs->primary_backup_match &&
           pfs->journal_class == PFS_JOURNAL_CLEAN &&
           pfs->journal_num == 0 && pfs->root_inode_type_valid &&
           pfs->root_directory_valid && pfs->root_directory_dot_found &&
           pfs->root_directory_dotdot_found &&
           !pfs->root_directory_entries_truncated &&
           pfs->root_directory_entries >= 2;
}

const char *psx1_capacity_name(media_capacity_class_t capacity)
{
    return capacity == MEDIA_CAPACITY_SETMAX_HIDDEN
               ? "PSX1 MEDIA"
               : format_test_capacity_name(capacity);
}

static void fail(psx1_result_t *result, int step, int return_value)
{
    result->format.stopped_on_error = 1;
    result->format.failed_step = step;
    result->format.failed_result = return_value;
}

void psx1_prepare(psx1_result_t *result)
{
    memset(result, 0, sizeof(*result));
    format_test_init(&result->format);
    installer_initialize_psx1(&result->installer);
    inspector_initialize(&result->before);
    inspector_scan_layout(&result->before);
    format_test_analyze_preflight(&result->before, &result->format);
    if (result->format.capacity_class == MEDIA_CAPACITY_OTHER &&
        result->format.physical_sector_count >= 78125000ULL * 9u / 10u &&
        result->format.physical_sector_count <= 78125000ULL * 11u / 10u)
        result->format.capacity_class = MEDIA_CAPACITY_SETMAX_HIDDEN;
    installer_scan_source(&result->installer);
    result->package_ready = result->installer.source_scan_valid;
    result->preflight_valid = result->format.preflight_pass &&
                              result->package_ready;
    result->hardware_verified_profile =
        result->format.capacity_class == MEDIA_CAPACITY_64_GB ||
        result->format.capacity_class == MEDIA_CAPACITY_128_GB ||
        result->format.capacity_class == MEDIA_CAPACITY_256_GB;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
}

void psx1_rescan_package(psx1_result_t *result)
{
    installer_release(&result->installer);
    installer_initialize_psx1(&result->installer);
    installer_scan_source(&result->installer);
    result->package_ready = result->installer.source_scan_valid;
    result->preflight_valid = result->format.preflight_pass &&
                              result->package_ready;
}

static void psx1_execute_internal(psx1_result_t *result, int recovery_mode)
{
    u64 total_start = GetTimerSystemTime();
    u64 start;
    unsigned int index;

    if (!result->preflight_valid)
        return;
    result->format.confirmation_received = 1;
    result->format.session_write_locked = 1;
    result->installer.confirmation_received = 1;

    if (recovery_mode) {
        if (cleanup_old_apa_headers(result) < 0) {
            fail(result, 1, result->format.recovery_cleanup_result);
            goto stopped;
        }
    }

    result->format.apa.attempted = 1;
    result->format.apa.return_value =
        run_format_job(1, "FAST APA FORMAT", FORMAT_JOB_APA, NULL,
                       &result->format.apa.duration_ms);
    if (result->format.apa.return_value < 0) {
        fail(result, 1, result->format.apa.return_value);
        goto stopped;
    }
    if (!validate_apa_after_format(result)) {
        result->format.recovery_available = !recovery_mode;
        fail(result, 1, -1);
        goto stopped;
    }

    for (index = 0; index < FORMAT_TEST_PFS_COUNT; ++index) {
        format_stage_t *stage = &result->format.pfs[index];
        char label[64];

        snprintf(label, sizeof(label), "FORMAT %s PFS", stage->name);
        stage->attempted = 1;
        stage->return_value =
            run_format_job(index + 2u, label, FORMAT_JOB_PFS,
                           stage->blockdev, &stage->duration_ms);
        if (stage->return_value < 0) {
            fail(result, (int)index + 2, stage->return_value);
            goto stopped;
        }
    }

    draw_operation(6, "VERIFYING", "RAW PFS VALIDATION",
                   "IN PROGRESS - DO NOT POWER OFF", 0);
    start = GetTimerSystemTime();
    reset_scan(&result->formatted);
    inspector_scan(&result->formatted);
    result->format.apa_layout_valid =
        format_test_exact_apa(&result->formatted);
    result->format.mbr_valid = format_test_mbr_valid(&result->formatted);
    result->format.raw_pfs_discovery_valid = 1;
    for (index = 0; index < FORMAT_TEST_PFS_COUNT; ++index) {
        const apa_entry_t *entry =
            find_entry(&result->formatted, result->format.pfs[index].name);

        result->format.pfs[index].raw_valid =
            entry != NULL && format_test_strict_pfs_valid(entry);
        if (!result->format.pfs[index].raw_valid)
            result->format.raw_pfs_discovery_valid = 0;
    }
    result->format.psx1_format_test_valid =
        result->format.apa_layout_valid && result->format.mbr_valid &&
        result->format.raw_pfs_discovery_valid;
    result->format.raw_validation_duration_ms =
        elapsed_ms(start, GetTimerSystemTime());
    result->format.raw_validation_return =
        result->format.psx1_format_test_valid ? 0 : -1;
    if (!result->format.psx1_format_test_valid) {
        fail(result, 6, -1);
        goto stopped;
    }

    draw_operation(7, "INSTALLING SYSTEM", "COPY AND VERIFY PACKAGE",
                   "IN PROGRESS - DO NOT POWER OFF", 0);
    installer_execute(&result->installer);
    result->all_package_files_valid =
        result->installer.system_files_install_valid;
    if (!result->all_package_files_valid) {
        fail(result, 7, result->installer.failure_return);
        goto stopped;
    }

    draw_operation(8, "VERIFYING", "FINAL PSX1 VALIDATION",
                   "IN PROGRESS - DO NOT POWER OFF", 0);
    reset_scan(&result->final_layout);
    inspector_scan(&result->final_layout);
    result->final_apa_valid = format_test_exact_apa(&result->final_layout);
    result->final_mbr_valid = format_test_mbr_valid(&result->final_layout);
    result->final_raw_pfs_valid = 1;
    for (index = 0; index < FORMAT_TEST_PFS_COUNT; ++index) {
        const apa_entry_t *entry = find_entry(
            &result->final_layout, result->format.pfs[index].name);

        result->final_pfs_valid[index] =
            entry != NULL && installed_pfs_valid(entry);
        if (!result->final_pfs_valid[index])
            result->final_raw_pfs_valid = 0;
    }
    result->psx1_storage_valid = result->final_apa_valid &&
        result->final_mbr_valid && result->final_raw_pfs_valid;
    result->psx1_installation_valid = result->psx1_storage_valid &&
        result->all_package_files_valid &&
        !result->format.stopped_on_error;
    if (!result->psx1_installation_valid)
        fail(result, 8, -1);

stopped:
    result->format.storage_activity_stopped = 1;
    result->format.total_operation_duration_ms =
        elapsed_ms(total_start, GetTimerSystemTime());
    if (!result->format.stopped_on_error) {
        draw_operation(8, "COMPLETE", "PSX1 INSTALLATION",
                       "COMPLETE", result->format.total_operation_duration_ms);
        DelayThread(750000);
    }
}

void psx1_execute(psx1_result_t *result)
{
    result->format.recovery_available = 0;
    psx1_execute_internal(result, 0);
}

int psx1_recovery_available(const psx1_result_t *result)
{
    return result->format.recovery_available &&
           result->format.apa.attempted &&
           result->format.apa.return_value >= 0 &&
           result->format.failed_step == 1;
}

void psx1_execute_recovery(psx1_result_t *result)
{
    unsigned int index;

    if (!psx1_recovery_available(result))
        return;
    result->format.recovery_confirmed = 1;
    result->format.recovery_available = 0;
    result->format.stopped_on_error = 0;
    result->format.failed_step = 0;
    result->format.failed_result = 0;
    result->format.storage_activity_stopped = 0;
    result->format.apa.attempted = 0;
    result->format.apa.return_value = 0;
    result->format.apa.duration_ms = 0;
    result->format.apa_layout_valid = 0;
    result->format.mbr_valid = 0;
    result->format.raw_validation_return = 0;
    result->format.raw_validation_duration_ms = 0;
    result->format.raw_pfs_discovery_valid = 0;
    result->format.psx1_format_test_valid = 0;
    for (index = 0; index < FORMAT_TEST_PFS_COUNT; ++index) {
        result->format.pfs[index].attempted = 0;
        result->format.pfs[index].return_value = 0;
        result->format.pfs[index].duration_ms = 0;
        result->format.pfs[index].raw_valid = 0;
    }
    result->all_package_files_valid = 0;
    result->final_apa_valid = 0;
    result->final_mbr_valid = 0;
    result->final_raw_pfs_valid = 0;
    result->psx1_storage_valid = 0;
    result->psx1_installation_valid = 0;
    memset(result->final_pfs_valid, 0, sizeof(result->final_pfs_valid));
    psx1_execute_internal(result, 1);
}

void psx1_release(psx1_result_t *result)
{
    installer_release(&result->installer);
}
