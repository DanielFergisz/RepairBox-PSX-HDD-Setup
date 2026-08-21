#include <delaythread.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "psx1_pipeline.h"
#include "ui.h"

#define PSX1_STAGE_COUNT 8u

static const int pfs_format_args[1] = {8192};
static unsigned char format_thread_stack[64 * 1024]
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
    ui_begin();
    ui_printf("RepairBox.pl PSX HDD Setup v1.0\n");
    ui_printf("PSX1 - First Revision\n\n");
    ui_inverse_status(phase);
    ui_printf("Step %u / %u\n%s\n\n", step, PSX1_STAGE_COUNT,
              operation);
    ui_inverse_status(status);
    if (duration_ms != 0)
        ui_printf("Elapsed %u.%03u s\n", duration_ms / 1000u,
                  duration_ms % 1000u);
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

void psx1_execute(psx1_result_t *result)
{
    u64 total_start = GetTimerSystemTime();
    u64 start;
    unsigned int index;

    if (!result->preflight_valid)
        return;
    result->format.confirmation_received = 1;
    result->format.session_write_locked = 1;
    result->installer.confirmation_received = 1;

    result->format.apa.attempted = 1;
    result->format.apa.return_value =
        run_format_job(1, "FAST APA FORMAT", FORMAT_JOB_APA, NULL,
                       &result->format.apa.duration_ms);
    if (result->format.apa.return_value < 0) {
        fail(result, 1, result->format.apa.return_value);
        goto stopped;
    }
    reset_scan(&result->formatted);
    inspector_scan_layout(&result->formatted);
    result->format.apa_layout_valid =
        format_test_exact_apa(&result->formatted);
    result->format.mbr_valid = format_test_mbr_valid(&result->formatted);
    if (!result->format.apa_layout_valid || !result->format.mbr_valid) {
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

void psx1_release(psx1_result_t *result)
{
    installer_release(&result->installer);
}
