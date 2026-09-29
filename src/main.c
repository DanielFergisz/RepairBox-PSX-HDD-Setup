#include <debug.h>
#include <errno.h>
#include <delaythread.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "activation.h"
#include "apps/apps_ui.h"
#include "build_profile.h"
#include "capacity_profile.h"
#include "direct_ready40.h"
#include "direct_ready40_multi.h"
#include "psx1_pipeline.h"
#include "source_media.h"
#include "storage.h"
#include "ui.h"
#include "iop_module_lookup.h"
#include "xfrom_repair.h"

#define USB_WAIT_TIMEOUT_MS 20000u
#define USB_POLL_INTERVAL_MS 250u
#define PROGRAM_TITLE RBX_PROGRAM_TITLE
#define ACTIVATION_STAGE (DR40_STAGE_COUNT + 1u)
#define PROGRAM_STAGE_COUNT ACTIVATION_STAGE

typedef enum selected_revision {
    REVISION_NONE = 0,
    REVISION_PSX1,
    REVISION_PSX2,
    REVISION_APPS_ONLY
} selected_revision_t;

typedef struct pad_diagnostics {
    int init_result;
    int open_result;
} pad_diagnostics_t;

static char pad_buffer[256] __attribute__((aligned(64)));
static unsigned char pad_start_stack[16384] __attribute__((aligned(64)));
static volatile int pad_start_stage;
static volatile int pad_start_done;
static int inherited_pad_id = -1;
static int inherited_sio2_id = -1;

static void initialize_pad(pad_diagnostics_t *pad)
{
    int result = source_media_prepare_controller_stack(&pad_start_stage);

    if (result < 0) {
        pad->init_result = result;
        return;
    }
    inherited_sio2_id = iop_module_find("sio2man");
    inherited_pad_id = iop_module_find("padman");
    if (inherited_sio2_id < 0) {
        pad_start_stage = 1;
        SifLoadModule("rom0:SIO2MAN", 0, NULL);
    }
    if (inherited_pad_id < 0) {
        pad_start_stage = 2;
        SifLoadModule("rom0:PADMAN", 0, NULL);
    }
    pad_start_stage = 3;
    pad->init_result = padInit(0);
    if (pad->init_result >= 0) {
        pad_start_stage = 4;
        pad->open_result = padPortOpen(0, 0, pad_buffer);
    }
}

static void pad_start_worker(void *context)
{
    initialize_pad((pad_diagnostics_t *)context);
    pad_start_done = 1;
    ExitDeleteThread();
}

static void draw_startup(const char *status)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n\n");
    ui_inverse_status(status);
    ui_printf("Please wait.\n");
    ui_sync();
}

static int initialize_pad_with_status(pad_diagnostics_t *pad)
{
    ee_thread_t thread;
    ee_thread_status_t current_thread;
    int thread_id;
    unsigned int elapsed;

    pad->init_result = -1;
    pad->open_result = -1;
    pad_start_done = 0;
    pad_start_stage = 0;
    inherited_pad_id = -1;
    inherited_sio2_id = -1;
    memset(&thread, 0, sizeof(thread));
    thread.func = pad_start_worker;
    thread.stack = pad_start_stack;
    thread.stack_size = sizeof(pad_start_stack);
    thread.gp_reg = &_gp;
    if (ReferThreadStatus(GetThreadId(), &current_thread) < 0 ||
        current_thread.current_priority >= 127)
        return -EIO;
    /* The status/timeout loop must preempt a polling libpad worker. */
    thread.initial_priority = current_thread.current_priority + 1;
    thread_id = CreateThread(&thread);
    if (thread_id < 0)
        return thread_id;
    if (StartThread(thread_id, pad) < 0) {
        DeleteThread(thread_id);
        return -EIO;
    }
    for (elapsed = 0; elapsed < 1000u; ++elapsed) {
        if (pad_start_done)
            return pad->init_result >= 0 && pad->open_result > 0 ? 0 : -EIO;
        DelayThread(10000);
    }
    /* Do not terminate a worker that may own pending RPC/DMA buffers.
     * The caller must remain alive and must not enter any HDD workflow. */
    return -ETIMEDOUT;
}

static void suspend_pad_for_source_drivers(void)
{
    (void)padPortClose(0, 0);
    DelayThread(100000);
}

static void resume_pad_after_source_drivers(pad_diagnostics_t *pad)
{
    unsigned int attempts;

    pad->open_result = padPortOpen(0, 0, pad_buffer);
    for (attempts = 0; attempts < 300u; ++attempts) {
        int state = padGetState(0, 0);

        if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1)
            break;
        DelayThread(10000);
    }
}

static void load_source_drivers(pad_diagnostics_t *pad)
{
    if (!source_media_needs_optional_modules())
        return;
    if (source_media_is_mmce()) {
        (void)source_media_load_optional_modules();
        return;
    }
    suspend_pad_for_source_drivers();
    (void)source_media_load_optional_modules();
    resume_pad_after_source_drivers(pad);
}

static int confirmation_chord(unsigned int held, unsigned int pressed)
{
    return source_media_driver_result() >= 0 &&
           (held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
           (pressed & PAD_CROSS) != 0;
}

static void draw_selector(void)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n\n");
    ui_printf("Select revision using the model on the rear label:\n");
    ui_set_position(52, 56);
    ui_printf("[ L1 ]  PSX1");
    ui_set_position(340, 56);
    ui_printf("[ R1 ]  PSX2");
    ui_set_position(52, 76);
    ui_printf("FIRST REVISION");
    ui_set_position(340, 76);
    ui_printf("SECOND REVISION");
    ui_set_position(52, 100);
    ui_printf("DESR-5000");
    ui_set_position(340, 100);
    ui_printf("DESR-5500");
    ui_set_position(52, 116);
    ui_printf("DESR-5100");
    ui_set_position(340, 116);
    ui_printf("DESR-5700");
    ui_set_position(52, 132);
    ui_printf("DESR-7000");
    ui_set_position(340, 132);
    ui_printf("DESR-7500");
    ui_set_position(52, 148);
    ui_printf("DESR-7100");
    ui_set_position(340, 148);
    ui_printf("DESR-7700");
    ui_set_position(UI_SAFE_LEFT, 176);
    ui_inverse_selector("L1 = PSX1", "\x1e  APPS", "R1 = PSX2");
    ui_set_position(UI_SAFE_LEFT, 208);
    ui_printf("O Exit");
    ui_sync();
}

static selected_revision_t select_revision(const pad_diagnostics_t *pad)
{
    struct padButtonStatus buttons;
    unsigned int old_buttons = 0;

    if (pad->init_result < 0 || pad->open_result <= 0)
        return REVISION_NONE;
    draw_selector();
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~old_buttons;

            old_buttons = current;
            if ((pressed & PAD_L1) != 0) {
                return REVISION_PSX1;
            } else if ((pressed & PAD_R1) != 0) {
                return REVISION_PSX2;
            } else if ((pressed & PAD_TRIANGLE) != 0) {
                return REVISION_APPS_ONLY;
            } else if ((pressed & PAD_CIRCLE) != 0) {
                return REVISION_NONE;
            }
        }
        DelayThread(16000);
    }
}

static void draw_preflight_wait(const char *revision, const char *status)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision);
    ui_inverse_status(status);
    ui_printf("Source: %s\n", source_media_label());
    ui_printf("Please wait. No disk write has started.\n");
    ui_sync();
}

static void draw_psx1_preflight(const psx1_result_t *result,
                                const xfrom_repair_result_t *xfrom,
                                int details)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    if (!details) {
        ui_printf("Source         %s\n", source_media_label());
        ui_printf("Hardware       %s\n",
                  result->format.device_readable ? "PASS" : "FAILED");
        if (result->format.capacity_class != MEDIA_CAPACITY_SETMAX_HIDDEN)
            ui_printf("Capacity       %s\n",
                      psx1_capacity_name(result->format.capacity_class));
        ui_printf("Storage        %s\n",
                  result->format.preflight_pass ? "READY" : "FAILED");
        ui_printf("System files   %s\n\n",
                  !result->installer.source_scan_attempted ? "NOT CHECKED"
                      : (result->package_ready ? "READY" : "FAILED"));
        ui_printf("XFROM files    %s\n",
                  xfrom->source_valid ? "READY" : "FAILED");
        if (result->preflight_valid) {
            ui_inverse_status("PSX1 READY TO INITIALIZE");
            ui_printf("Hold L1 + R1 and press X.\n");
        } else {
            ui_inverse_status("STOP - CHECKING FAILED");
        }
        ui_printf("RIGHT: Details   TRIANGLE: Rescan\n");
    } else {
        ui_printf("Details\n\n");
        ui_printf("State: %s\n",
                  format_test_state_name(result->format.pre_format_state));
        if (result->format.capacity_class != MEDIA_CAPACITY_SETMAX_HIDDEN) {
            ui_printf("Sectors: 0x%llX\n",
                      result->format.physical_sector_count);
        }
        ui_printf("LBA48: %s\n",
                  result->format.lba48_supported ? "YES" : "NO");
        ui_printf("APA entries: %u  formatver=%d\n",
                  result->before.apa_count, result->before.hdd_formatver);
        ui_printf("Package: %u files  %llu bytes\n",
                  result->installer.source_file_count,
                  result->installer.source_total_bytes);
        ui_printf("Source: %s\n", source_media_label());
        ui_printf("XFROM: %u files %u bytes target=%s\n",
                  xfrom->file_count, xfrom->total_bytes,
                  xfrom->xfrom_partition_accessible
                      ? "PASS"
                      : (xfrom->xfrom_partition_missing
                             ? "CREATE" : "FAIL"));
        ui_printf("XFROM cleanup: %u removed  %s\n",
                  xfrom->cleanup_removed,
                  xfrom->cleanup_valid ? "PASS" : "PENDING");
        ui_printf("Source root: %d  close=%d  wait=%u ms\n",
                  result->usb_root_last_dopen,
                  result->usb_root_close_result,
                  result->usb_wait_ms);
        if (!result->package_ready) {
            ui_printf("Package error: %.26s (%d)\n",
                      result->installer.failure_operation,
                      result->installer.failure_return);
            ui_printf("%.18s%.30s\n",
                      result->installer.failure_partition,
                      result->installer.failure_path);
        }
        if (!xfrom->source_valid && xfrom->failure_return < 0)
            ui_printf("XFROM error: %d %.28s\n", xfrom->failure_return,
                      xfrom->failure_operation);
        ui_set_position(UI_SAFE_LEFT, 184);
        ui_printf("LEFT/RIGHT: Back   TRIANGLE: Rescan\n");
    }
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("O Exit\n");
    ui_sync();
}

static void draw_psx1_source_wait(const psx1_result_t *result)
{
    (void)result;
    draw_preflight_wait("PSX1 - First Revision", "CHECKING SYSTEM PACKAGE");
}

static void wait_for_psx1_source(psx1_result_t *result)
{
    result->usb_wait_attempted = 1;
    result->usb_ready = 0;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
    result->usb_wait_attempts = 0;
    result->usb_wait_ms = 0;
    draw_psx1_source_wait(result);
    result->usb_root_last_dopen =
        source_media_select(SOURCE_MEDIA_PSX1_SYSTEM);
    result->usb_wait_attempts = 1;
    if (result->usb_root_last_dopen >= 0) {
        result->usb_root_close_result = 0;
        result->usb_ready = 1;
        ++result->usb_rescan_count;
        psx1_rescan_package(result);
    }
}

static void draw_psx1_result(const psx1_result_t *result,
                             const xfrom_repair_result_t *xfrom,
                             int xfrom_return)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    if (xfrom_return != 0) {
        ui_inverse_status("XFROM RESTORE FAILED");
        ui_printf("XFROM return=%d\n", xfrom_return);
        ui_printf("%.46s\n%.54s\n", xfrom->failure_operation,
                  xfrom->failure_path);
        ui_printf("HDD installation was not started.\n");
    } else if (result->psx1_installation_valid) {
        ui_inverse_status("PSX1 INSTALLATION COMPLETE");
        ui_inverse_status("FULL POWER OFF REQUIRED");
        ui_printf("XFROM, storage and system files: PASS\n");
        ui_printf("XFROM cleanup: %u removed\n",
                  xfrom->cleanup_removed);
        ui_printf("Power off the PSX.\n");
        ui_printf("Disconnect AC power before restarting.\n");
        ui_printf("Reconnect and start the PSX normally.\n");
    } else {
        ui_inverse_status("FAILED - STOPPED");
        ui_printf("Step=%d return=%d\n",
                  result->format.failed_step,
                  result->format.failed_result);
        if (result->format.recovery_cleanup_attempted) {
            ui_printf("APA cleanup: %u/%u return=%d\n",
                      result->format.recovery_cleanup_verified_slots,
                      result->format.recovery_cleanup_slots,
                      result->format.recovery_cleanup_result);
            if (result->format.recovery_cleanup_result < 0)
                ui_printf("Cleanup failed at LBA 0x%08X\n",
                          result->format.recovery_cleanup_failure_lba);
        } else if (psx1_recovery_available(result)) {
            ui_printf("Fast format did not verify after %u scans.\n",
                      result->format.post_format_scan_attempts);
            ui_printf("Hold L1 + R1 and press X:\n");
            ui_printf("APA cleanup + format retry\n");
        }
        if (result->installer.failure_return != 0) {
            ui_printf("Operation: %s\n",
                      result->installer.failure_operation);
            ui_printf("Path: %s%s\n",
                      result->installer.failure_partition,
                      result->installer.failure_path);
            if (result->installer.failure_attempt != 0)
                ui_printf("Byte=%llu attempt=%u/%u\n",
                          result->installer.failure_offset,
                          result->installer.failure_attempt, 2u);
        }
        ui_printf("No later stage was attempted.\n");
    }
    ui_draw_repairbox_logo(408, 168);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf(psx1_recovery_available(result) ? "O Exit\n" : "X/O Exit\n");
    ui_sync();
}

typedef struct xfrom_ui_progress_state {
    const char *revision;
    int write_mode;
    int screen_ready;
    int last_phase;
    u32 last_file_index;
    u32 last_percent;
} xfrom_ui_progress_state_t;

typedef struct psx2_ui_progress_state {
    unsigned int stage;
    int screen_ready;
} psx2_ui_progress_state_t;

static void ui_progress_line(int y, const char *text)
{
    ui_set_position(UI_SAFE_LEFT, y);
    ui_printf("%-54.54s", text != NULL ? text : "");
}

static void xfrom_progress_begin(xfrom_ui_progress_state_t *state,
                                 const char *revision, int write_mode);
static void xfrom_progress_callback(xfrom_progress_phase_t phase,
                                    const char *path, u32 file_index,
                                    u32 file_count, u32 completed, u32 total,
                                    void *context);

static void run_psx1_ui(pad_diagnostics_t *pad)
{
    static psx1_result_t result;
    static xfrom_repair_result_t xfrom;
    static storage_diagnostics_t xfrom_diagnostics;
    struct padButtonStatus buttons;
    unsigned int old_buttons = 0;
    int details = 0;
    int finished = 0;
    int xfrom_return = -1;
    xfrom_ui_progress_state_t xfrom_progress;

    draw_preflight_wait("PSX1 - First Revision", "CHECKING STORAGE AND PACKAGE");
    psx1_prepare(&result);
    if (source_media_needs_optional_modules()) {
        load_source_drivers(pad);
        (void)source_media_select(SOURCE_MEDIA_PSX1_SYSTEM);
        psx1_rescan_package(&result);
    }
    if (!result.package_ready)
        wait_for_psx1_source(&result);
    storage_initialize_xfrom_only(&xfrom_diagnostics);
    xfrom_progress_begin(&xfrom_progress, "PSX1 - First Revision", 0);
    xfrom_repair_validate_source(
        &xfrom, XFROM_REPAIR_PSX1, xfrom_progress_callback,
        &xfrom_progress);
    result.preflight_valid = result.preflight_valid &&
        xfrom.source_valid && xfrom.xfrom_root_accessible;
    draw_psx1_preflight(&result, &xfrom, details);
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~old_buttons;

            old_buttons = current;
            if (!finished && (pressed & PAD_TRIANGLE) != 0) {
                wait_for_psx1_source(&result);
                xfrom_progress_begin(&xfrom_progress,
                                     "PSX1 - First Revision", 0);
                xfrom_repair_validate_source(
                    &xfrom, XFROM_REPAIR_PSX1, xfrom_progress_callback,
                    &xfrom_progress);
                result.preflight_valid = result.preflight_valid &&
                    xfrom.source_valid && xfrom.xfrom_root_accessible;
                details = 0;
                draw_psx1_preflight(&result, &xfrom, details);
            } else if (!finished &&
                (pressed & (PAD_LEFT | PAD_RIGHT)) != 0) {
                details ^= 1;
                draw_psx1_preflight(&result, &xfrom, details);
            } else if (!finished && result.preflight_valid &&
                       confirmation_chord(current, pressed)) {
                xfrom_progress_begin(&xfrom_progress,
                                     "PSX1 - First Revision", 1);
                xfrom_return = xfrom_repair_execute(
                    &xfrom, 1, 1, xfrom_progress_callback,
                    &xfrom_progress);
                if (xfrom_return == 0)
                    psx1_execute(&result);
                finished = 1;
                draw_psx1_result(&result, &xfrom, xfrom_return);
            } else if (finished && psx1_recovery_available(&result) &&
                       confirmation_chord(current, pressed)) {
                psx1_execute_recovery(&result);
                draw_psx1_result(&result, &xfrom, xfrom_return);
            } else if ((pressed & PAD_CIRCLE) != 0 ||
                       (finished && (pressed & PAD_CROSS) != 0)) {
                break;
            }
        }
        DelayThread(16000);
    }
    psx1_release(&result);
}

static void draw_source_wait(const dr40_result_t *result)
{
    (void)result;
    draw_preflight_wait("PSX2 - Second Revision", "CHECKING SYSTEM PACKAGE");
}

static void wait_for_system_source(dr40_result_t *result)
{
    result->usb_wait_attempted = 1;
    result->usb_ready = 0;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
    result->usb_wait_attempts = 0;
    result->usb_wait_ms = 0;
    draw_source_wait(result);
    result->usb_root_last_dopen =
        source_media_select(SOURCE_MEDIA_PSX2_SYSTEM);
    result->usb_wait_attempts = 1;
    if (result->usb_root_last_dopen >= 0) {
        result->usb_root_close_result = 0;
        result->usb_ready = 1;
    }
}

static void rescan_psx2_preflight(dr40_result_t *result,
                                  storage_diagnostics_t *diagnostics,
                                  capacity_profile_t *profile)
{
    ++result->usb_rescan_count;
    wait_for_system_source(result);
    storage_refresh_layout_diagnostics(diagnostics);
    *profile = capacity_profile_detect(
        (u32)diagnostics->dvr_hdd.max_lba48_result,
        diagnostics->dvr_hdd.is_lba48_result);
    if (*profile == CAPACITY_PROFILE_256_VERIFIED)
        dr40_scan_preflight(result, diagnostics);
    else
        dr40_multi_scan_preflight(result, diagnostics);
}

static const char *psx2_storage_status(const dr40_result_t *result)
{
    if (result->mode == DR40_MODE_INITIALIZE)
        return "NEEDS SETUP";
    if (result->mode == DR40_MODE_SET_BOUNDARY)
        return "BOUNDARY SETUP";
    return "STOP";
}

static void draw_psx2_preflight(const dr40_result_t *result,
                                const activation_result_t *activation,
                                const xfrom_repair_result_t *xfrom,
                                capacity_profile_t profile, int details,
                                const storage_diagnostics_t *diagnostics)
{
    (void)diagnostics;

    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    if (!details) {
        ui_printf("Source         %s\n", source_media_label());
        ui_printf("Hardware       %s\n",
                  result->hardware_profile_valid ? "PASS" : "FAILED");
        ui_printf("Capacity       %s\n", capacity_profile_name(profile));
        ui_printf("Storage        %s\n", psx2_storage_status(result));
        if (result->mode == DR40_MODE_SET_BOUNDARY) {
            ui_printf("System package NOT REQUIRED THIS PASS\n");
            ui_printf("Activation     %s\n",
                      activation->already_armed ? "PRESERVED"
                          : (activation->replacement_required
                                 ? "REPLACE ON COMPLETE" : "READY"));
        } else {
            ui_printf("System files   %s\n",
                      !result->installer.source_scan_attempted ? "NOT CHECKED"
                          : (result->source_ready ? "READY" : "FAILED"));
            ui_printf("Bootstrap      %s\n",
                      !result->bootstrap.validation_attempted ? "NOT CHECKED"
                          : (result->bootstrap_ready ? "READY" : "FAILED"));
            ui_printf("XFROM files    %s\n",
                      xfrom->source_valid ? "READY" : "FAILED");
            ui_printf("Activation     %s\n",
                      activation->already_armed
                          ? "PENDING - PRESERVE"
                          : (activation->replacement_required
                                 ? (activation->current_accessible
                                        ? "REPLACE ON COMPLETE"
                                        : "CREATE ON COMPLETE")
                          : (activation->generation_valid
                                 ? "READY" : "FAILED")));
        }
        if (result->preflight_valid) {
            ui_inverse_status(result->mode == DR40_MODE_SET_BOUNDARY
                                  ? "READY TO PREPARE BOUNDARY"
                                  : (activation->already_armed
                                         ? "PSX2 READY TO REINSTALL"
                                         : "PSX2 READY TO INITIALIZE"));
            ui_printf("Hold L1 + R1 and press X.\n");
        } else {
            ui_inverse_status("STOP - CHECKING FAILED");
        }
        ui_printf("RIGHT: Details   TRIANGLE: Rescan\n");
    } else {
        /* Completed preflight snapshot: opening Details does not scan again. */
        const installer_result_t *files = &result->installer;
        ui_printf("Details\n\n");
        ui_printf("%.54s\n", source_media_psx2_system_root());
        ui_printf("Partitions: %u/%u   Files: %u\n",
                  files->source_partition_count, files->partition_count,
                  files->source_file_count);
        ui_printf("Source: %s %s (%d)\n", source_media_label(),
                  source_media_resolution_name(),
                  source_media_resolution_result());
        ui_printf("Package error: %d\n", files->failure_return);
        if (files->failure_return < 0) {
            ui_printf("%.48s\n", files->failure_operation);
            ui_printf("%.20s %.28s\n",
                      files->failure_partition, files->failure_path);
        }
        ui_printf("Bootstrap: %s  Stack: %s\n",
                  result->bootstrap_ready ? "READY" : "NOT READY",
                  result->stack_ready ? "PASS" : "FAILED");
        ui_printf("XFROM: %u files %u bytes target=%s\n",
                  xfrom->file_count, xfrom->total_bytes,
                  xfrom->xfrom_partition_accessible
                      ? "PASS"
                      : (xfrom->xfrom_partition_missing
                             ? "CREATE" : "FAIL"));
        ui_printf("XFROM cleanup: %u removed  %s\n",
                  xfrom->cleanup_removed,
                  xfrom->cleanup_valid ? "PASS" : "PENDING");
        if (!xfrom->source_valid && xfrom->failure_return < 0)
            ui_printf("XFROM error: %d %.34s\n", xfrom->failure_return,
                      xfrom->failure_operation);
        ui_printf("Bootflag: %s size=%u sha=%s u=%u\n",
                  activation->already_armed ? "PENDING"
                      : (activation->replacement_required
                             ? (activation->current_accessible
                                    ? "REPLACE" : "CREATE")
                             : (result->bootflag_normal
                                                    ? "NORMAL" : "BLOCKED")),
                  result->bootflag.size,
                  result->bootflag.sha1_valid ? "OK" : "BAD",
                  result->bootflag.unknown_key_count);
        ui_printf("LEFT/RIGHT: Back\n");
    }
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("O Exit\n");
    ui_sync();
}

static void psx2_progress_callback(unsigned int stage, const char *label,
                                   dr40_progress_phase_t phase,
                                   int return_value, u32 duration_ms,
                                   void *context)
{
    psx2_ui_progress_state_t *state = context;
    char line[96];
    int rebuild_screen;

    rebuild_screen = state == NULL || !state->screen_ready ||
                     state->stage != stage ||
                     (stage == 11u && phase != DR40_PROGRESS_BEGIN);
    if (rebuild_screen) {
        ui_begin();
        ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
        if (state != NULL) {
            state->stage = stage;
            state->screen_ready = 1;
        }
    }
    ui_set_position(UI_SAFE_LEFT, 52);
    if (stage <= 9)
        ui_inverse_status("PREPARING STORAGE");
    else if (stage <= 11)
        ui_inverse_status("INSTALLING SYSTEM");
    else
        ui_inverse_status("VERIFYING");
    snprintf(line, sizeof(line), "Step %u / %u", stage,
             PROGRAM_STAGE_COUNT);
    ui_progress_line(76, line);
    ui_progress_line(100, label);
    ui_set_position(UI_SAFE_LEFT, 132);
    if (phase == DR40_PROGRESS_BEGIN) {
        ui_inverse_status("IN PROGRESS - DO NOT POWER OFF");
    } else if (phase == DR40_PROGRESS_VALIDATE) {
        ui_inverse_status("VERIFYING");
    } else if (phase == DR40_PROGRESS_COMPLETE) {
        ui_inverse_status("COMPLETE");
    } else {
        ui_inverse_status("FAILED");
    }
    if (phase != DR40_PROGRESS_BEGIN) {
        snprintf(line, sizeof(line), "Return=%d  duration=%u ms",
                 return_value, duration_ms);
        ui_progress_line(156, line);
    } else {
        ui_progress_line(156, "");
    }
    ui_sync();
    if (phase == DR40_PROGRESS_BEGIN)
        DelayThread(150000);
}

static void draw_activation_progress(dr40_progress_phase_t phase,
                                     int return_value, u32 duration_ms,
                                     const char *operation)
{
    char line[96];

    if (phase == DR40_PROGRESS_BEGIN) {
        ui_begin();
        ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    }
    ui_set_position(UI_SAFE_LEFT, 52);
    ui_inverse_status("ACTIVATING");
    snprintf(line, sizeof(line), "Step %u / %u", ACTIVATION_STAGE,
             PROGRAM_STAGE_COUNT);
    ui_progress_line(76, line);
    ui_progress_line(100, operation);
    ui_set_position(UI_SAFE_LEFT, 132);
    if (phase == DR40_PROGRESS_BEGIN)
        ui_inverse_status("IN PROGRESS - DO NOT POWER OFF");
    else
        ui_inverse_status(phase == DR40_PROGRESS_COMPLETE
                              ? "COMPLETE" : "FAILED");
    if (phase != DR40_PROGRESS_BEGIN) {
        snprintf(line, sizeof(line), "Return=%d  duration=%u ms",
                 return_value, duration_ms);
        ui_progress_line(156, line);
    } else {
        ui_progress_line(156, "");
    }
    ui_sync();
    if (phase == DR40_PROGRESS_BEGIN)
        DelayThread(150000);
}

static const char *xfrom_progress_label(xfrom_progress_phase_t phase)
{
    switch (phase) {
        case XFROM_PROGRESS_VALIDATE_BACKUP:
            return "CHECKING SOURCE PACKAGE";
        case XFROM_PROGRESS_PREPARE_TARGET:
            return "PREPARING XFROM";
        case XFROM_PROGRESS_SCAN_CLEANUP:
            return "PREPARING XFROM";
        case XFROM_PROGRESS_REMOVE_EXTRAS:
            return "CLEANING OLD XFROM CONTENTS";
        case XFROM_PROGRESS_VERIFY_CLEANUP:
            return "PREPARING XFROM";
        case XFROM_PROGRESS_RELOAD_BACKUP:
            return "OVERWRITING XFROM FROM SOURCE";
        case XFROM_PROGRESS_WRITE_TARGET:
            return "OVERWRITING XFROM FROM SOURCE";
        case XFROM_PROGRESS_VERIFY_TARGET:
            return "VERIFYING XFROM";
        default:
            return "PROCESSING XFROM";
    }
}

static const char *xfrom_progress_filename(const char *path)
{
    const char *slash;

    if (path == NULL || path[0] == '\0')
        return "XFROM";
    slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static void xfrom_progress_begin(xfrom_ui_progress_state_t *state,
                                 const char *revision, int write_mode)
{
    memset(state, 0, sizeof(*state));
    state->revision = revision;
    state->write_mode = write_mode;
    state->last_phase = -1;
    state->last_percent = 101u;
}

static void xfrom_progress_callback(xfrom_progress_phase_t phase,
                                    const char *path, u32 file_index,
                                    u32 file_count, u32 completed, u32 total,
                                    void *context)
{
    xfrom_ui_progress_state_t *state = context;
    char bar[25];
    unsigned int percent;
    unsigned int filled;
    unsigned int index;
    char line[128];
    u32 display_completed;
    u32 display_total;
    int progress_group;
    int stage_changed;

    if (state == NULL)
        return;
    if (total > 0u && completed > total)
        completed = total;
    display_completed = completed;
    display_total = total;
    progress_group = (int)phase;
    if (phase == XFROM_PROGRESS_RELOAD_BACKUP) {
        display_total = total * 2u;
        progress_group = 100;
    } else if (phase == XFROM_PROGRESS_WRITE_TARGET) {
        display_completed = total + completed;
        display_total = total * 2u;
        progress_group = 100;
    }
    percent = display_total == 0u
                  ? 100u
                  : (unsigned int)(((u64)display_completed * 100u) /
                                   display_total);
    if (percent > 100u)
        percent = 100u;
    stage_changed = state->last_phase != progress_group ||
                    state->last_file_index != file_index;
    if (!stage_changed && display_completed != display_total &&
        percent < state->last_percent + 5u)
        return;

    filled = percent * 24u / 100u;
    for (index = 0; index < 24u; ++index)
        bar[index] = index < filled ? '#' : '-';
    bar[24] = '\0';

    if (!state->screen_ready) {
        ui_begin();
        ui_printf(PROGRAM_TITLE "\n%s\n\n", state->revision);
        ui_inverse_status(state->write_mode
                              ? "REPAIRING XFROM - DO NOT POWER OFF"
                              : "CHECKING SOURCE PACKAGE - READ ONLY");
        state->screen_ready = 1;
    }
    ui_progress_line(76, xfrom_progress_label(phase));
    snprintf(line, sizeof(line), "Source: %.40s", source_media_label());
    ui_progress_line(100, line);
    if (file_index > 0u) {
        snprintf(line, sizeof(line), "File %u / %u: %.32s", file_index,
                 file_count, xfrom_progress_filename(path));
    } else {
        snprintf(line, sizeof(line), "Target: %.40s",
                 xfrom_progress_filename(path));
    }
    ui_progress_line(124, line);
    snprintf(line, sizeof(line), "[%s] %u%%", bar, percent);
    ui_progress_line(148, line);
    if (display_total > 0u) {
        snprintf(line, sizeof(line), "Progress: %u / %u bytes",
                 display_completed, display_total);
        ui_progress_line(172, line);
    } else {
        ui_progress_line(172, "");
    }
    ui_progress_line(184,
                     "The display advances after processed data blocks.");
    ui_progress_line(196, state->write_mode
                              ? ""
                              : "No disk write has started.");
    ui_sync();

    state->last_phase = progress_group;
    state->last_file_index = file_index;
    state->last_percent = percent;
}

static void draw_psx2_result(const dr40_result_t *result,
                             const activation_result_t *activation,
                             const xfrom_repair_result_t *xfrom,
                             int xfrom_return,
                             int activation_return,
                             capacity_profile_t profile)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    ui_printf("Profile: %s\n", capacity_profile_name(profile));
    if (result->mode == DR40_MODE_SET_BOUNDARY && result->setmax40.valid) {
        ui_inverse_status("40 GB BOUNDARY SET");
        ui_inverse_status("FULL POWER CYCLE REQUIRED");
        ui_printf("Run this same ELF again.\n");
    } else if (result->direct_ready40_storage_valid &&
               activation_return == 0 &&
               activation->sony_activation_armed) {
        ui_inverse_status("PSX2 INSTALLATION COMPLETE");
        ui_inverse_status("FULL POWER OFF REQUIRED");
        ui_printf("XFROM, storage and activation: PASS\n");
        ui_printf("XFROM written=%u deferred=%u\n",
                  xfrom->files_written, xfrom->files_deferred);
        ui_printf("XFROM cleanup: %u removed\n",
                  xfrom->cleanup_removed);
        if (activation->already_armed)
            ui_printf("Existing XFROM activation preserved.\n");
        ui_printf("Power off the PSX.\n");
        ui_printf("Disconnect AC power before restarting.\n");
        ui_printf("Reconnect and start the PSX normally.\n");
    } else if (result->mode == DR40_MODE_INITIALIZE && xfrom_return != 0) {
        ui_inverse_status("XFROM REPAIR FAILED");
        ui_printf("HDD installation was not started.\n");
        ui_printf("XFROM return=%d\n", xfrom_return);
        ui_printf("%.46s\n%.54s\n", xfrom->failure_operation,
                  xfrom->failure_path);
        ui_printf("Activation was not written.\n");
    } else if (result->direct_ready40_storage_valid) {
        ui_inverse_status("ACTIVATION FAILED");
        ui_printf("Storage pipeline: PASS\n");
        ui_printf("Activation return=%d\n", activation_return);
    } else {
        ui_inverse_status("FAILED - STOPPED");
        ui_printf("Phase=%d return=%d\n", result->failed_phase,
                  result->failure_return);
        ui_printf("Operation: %s\n", result->failure_operation);
        ui_printf("Path: %s%s\n", result->failure_partition,
                  result->failure_path);
        if (result->installer.failure_attempt != 0)
            ui_printf("Byte=%llu attempt=%u/%u\n",
                      result->installer.failure_offset,
                      result->installer.failure_attempt, 2u);
    }
    ui_draw_repairbox_logo(408, 168);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit\n");
    ui_sync();
}

static void run_psx2_ui(storage_diagnostics_t *diagnostics)
{
    struct padButtonStatus buttons;
    dr40_result_t result;
    static activation_result_t activation;
    static xfrom_repair_result_t xfrom;
    unsigned int old_buttons = 0;
    int finished = 0;
    int details = 0;
    int activation_return = -1;
    int xfrom_return = -1;
    int read_result = 0;
    xfrom_ui_progress_state_t xfrom_progress;
    psx2_ui_progress_state_t storage_progress;
    capacity_profile_t profile = capacity_profile_detect(
        (u32)diagnostics->dvr_hdd.max_lba48_result,
        diagnostics->dvr_hdd.is_lba48_result);

    memset(&storage_progress, 0, sizeof(storage_progress));

    dr40_initialize(&result, diagnostics);
    activation_initialize(&activation);
    activation_read_current(&activation);
    activation_prepare(&activation);
    xfrom_progress_begin(&xfrom_progress, "PSX2 - Second Revision", 0);
    xfrom_repair_validate_source(
        &xfrom, XFROM_REPAIR_PSX2, xfrom_progress_callback,
        &xfrom_progress);
    bootflag_ro_allow_pending_reinstall(activation.already_armed);
    if (profile == CAPACITY_PROFILE_256_VERIFIED)
        dr40_scan_preflight(&result, diagnostics);
    else
        dr40_multi_scan_preflight(&result, diagnostics);
    if (result.mode == DR40_MODE_INITIALIZE)
        result.preflight_valid = result.preflight_valid &&
            (activation.generation_valid || activation.already_armed) &&
            xfrom.source_valid && xfrom.xfrom_root_accessible;
    draw_psx2_preflight(&result, &activation, &xfrom, profile, details,
                        diagnostics);
    for (;;) {
        int state = padGetState(0, 0);

        read_result = 0;
        if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1)
            read_result = padRead(0, 0, &buttons);
        if (read_result > 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~old_buttons;

            old_buttons = current;
            if (!finished && (pressed & PAD_TRIANGLE) != 0) {
                activation_initialize(&activation);
                activation_read_current(&activation);
                activation_prepare(&activation);
                bootflag_ro_allow_pending_reinstall(
                    activation.already_armed);
                rescan_psx2_preflight(&result, diagnostics, &profile);
                xfrom_progress_begin(&xfrom_progress,
                                     "PSX2 - Second Revision", 0);
                xfrom_repair_validate_source(
                    &xfrom, XFROM_REPAIR_PSX2, xfrom_progress_callback,
                    &xfrom_progress);
                if (result.mode == DR40_MODE_INITIALIZE)
                    result.preflight_valid = result.preflight_valid &&
                        (activation.generation_valid ||
                         activation.already_armed) &&
                        xfrom.source_valid && xfrom.xfrom_root_accessible;
                details = 0;
                draw_psx2_preflight(&result, &activation, &xfrom, profile,
                                    details, diagnostics);
            } else if (!finished &&
                       (pressed & (PAD_LEFT | PAD_RIGHT)) != 0) {
                details ^= 1;
                draw_psx2_preflight(&result, &activation, &xfrom, profile,
                                    details, diagnostics);
            } else if (!finished && result.preflight_valid &&
                       confirmation_chord(current, pressed)) {
                result.confirmation_received = 1;
                if (result.mode == DR40_MODE_INITIALIZE) {
                    xfrom_progress_begin(&xfrom_progress,
                                         "PSX2 - Second Revision", 1);
                    xfrom_return = xfrom_repair_execute(
                        &xfrom, 1, result.confirmation_received,
                        xfrom_progress_callback, &xfrom_progress);
                }
                if (result.preflight_valid &&
                    (result.mode != DR40_MODE_INITIALIZE ||
                     xfrom_return == 0)) {
                    if (profile == CAPACITY_PROFILE_256_VERIFIED)
                        dr40_execute(&result, diagnostics,
                                     psx2_progress_callback,
                                     &storage_progress);
                    else
                        dr40_multi_execute(&result, diagnostics,
                                           psx2_progress_callback,
                                           &storage_progress);
                }
                if (result.direct_ready40_storage_valid &&
                    xfrom_return == 0 && xfrom.repair_valid) {
                    const char *activation_operation =
                        activation.already_armed
                            ? "VERIFY EXISTING XFROM 40/1"
                            : "ACTIVATE XFROM 40/1";

                    draw_activation_progress(DR40_PROGRESS_BEGIN, -1, 0,
                                             activation_operation);
                    activation_return = activation.already_armed
                        ? activation_verify_existing_pending(
                              &activation, 1,
                              result.confirmation_received)
                        : activation_arm_pending(
                              &activation, 1,
                              result.confirmation_received);
                    draw_activation_progress(
                        activation_return == 0
                            ? DR40_PROGRESS_COMPLETE
                            : DR40_PROGRESS_FAILED,
                        activation_return,
                        activation.activation_duration_ms,
                        activation_operation);
                }
                finished = 1;
                draw_psx2_result(&result, &activation, &xfrom, xfrom_return,
                                 activation_return, profile);
            } else if ((pressed & PAD_CIRCLE) != 0 ||
                       (finished && (pressed & PAD_CROSS) != 0)) {
                break;
            }
        }
        DelayThread(16000);
    }
    dr40_release(&result);
}

int main(int argc, char **argv)
{
    static storage_diagnostics_t psx2_diagnostics;
    pad_diagnostics_t pad;
    selected_revision_t revision;
    int pad_start_result;

    init_scr();
    ui_init();
    draw_startup("STARTING");
    SifInitRpc(0);
    source_media_detect_preferred(argc, argv);
    pad_start_result = initialize_pad_with_status(&pad);
    if (pad_start_result < 0) {
        draw_startup(pad_start_result == -ETIMEDOUT
                         ? "CONTROLLER START TIMEOUT" : "CONTROLLER START FAILED");
        ui_printf("Error: %d\n", pad_start_result);
        ui_printf("Power off the console to exit.\n");
        ui_sync();
        for (;;)
            DelayThread(1000000);
    }
    revision = select_revision(&pad);
    if (revision != REVISION_NONE)
        (void)source_media_prepare_unrecognized();
    if (revision == REVISION_PSX1) {
        run_psx1_ui(&pad);
    } else if (revision == REVISION_PSX2) {
        draw_preflight_wait("PSX2 - Second Revision", "CHECKING STORAGE AND PACKAGE");
        storage_initialize(&psx2_diagnostics);
        load_source_drivers(&pad);
        storage_initialize_xfrom(&psx2_diagnostics);
        (void)source_media_select(SOURCE_MEDIA_PSX2_SYSTEM);
        storage_refresh_layout_diagnostics(&psx2_diagnostics);
        run_psx2_ui(&psx2_diagnostics);
        storage_release(&psx2_diagnostics);
    } else if (revision == REVISION_APPS_ONLY) {
        apps_ui_run();
    }
    padPortClose(0, 0);
    return 0;
}
