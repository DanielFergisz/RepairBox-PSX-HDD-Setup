#include <debug.h>
#include <delaythread.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "activation.h"
#include "capacity_profile.h"
#include "direct_ready40.h"
#include "direct_ready40_multi.h"
#include "psx1_pipeline.h"
#include "storage.h"
#include "ui.h"

#define USB_WAIT_TIMEOUT_MS 20000u
#define USB_POLL_INTERVAL_MS 250u
#define PROGRAM_TITLE "RepairBox.pl PSX HDD Setup v1.0"
#define PROGRAM_STAGE_COUNT (DR40_STAGE_COUNT + 1u)

typedef enum selected_revision {
    REVISION_NONE = 0,
    REVISION_PSX1,
    REVISION_PSX2
} selected_revision_t;

typedef struct pad_diagnostics {
    int init_result;
    int open_result;
} pad_diagnostics_t;

static char pad_buffer[256] __attribute__((aligned(64)));

static void initialize_pad(pad_diagnostics_t *pad)
{
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    pad->init_result = padInit(0);
    pad->open_result = padPortOpen(0, 0, pad_buffer);
}

static int confirmation_chord(unsigned int held, unsigned int pressed)
{
    return (held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
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
    ui_set_position(UI_SAFE_LEFT, 172);
    ui_inverse_status("L1 = PSX1                 R1 = PSX2");
    ui_set_position(UI_SAFE_LEFT, 204);
    ui_printf("O Exit");
    ui_sync();
}

static selected_revision_t select_revision(const pad_diagnostics_t *pad)
{
    struct padButtonStatus buttons;
    unsigned int old_buttons = 0;

    if (pad->init_result == 0 || pad->open_result == 0)
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
            } else if ((pressed & PAD_CIRCLE) != 0) {
                return REVISION_NONE;
            }
        }
        DelayThread(16000);
    }
}

static void draw_psx1_preflight(const psx1_result_t *result, int details)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    if (!details) {
        ui_printf("Hardware       %s\n",
                  result->format.device_readable ? "PASS" : "FAILED");
        if (result->format.capacity_class != MEDIA_CAPACITY_SETMAX_HIDDEN)
            ui_printf("Capacity       %s\n",
                      psx1_capacity_name(result->format.capacity_class));
        ui_printf("Storage        %s\n",
                  result->format.preflight_pass ? "READY" : "FAILED");
        ui_printf("System package %s\n\n",
                  result->package_ready ? "READY" : "FAILED");
        if (result->preflight_valid) {
            ui_inverse_status("PSX1 READY TO INITIALIZE");
            ui_printf("Hold L1 + R1 and press X.\n");
        } else {
            ui_inverse_status("STOP - CHECKING FAILED");
        }
        ui_printf("RIGHT: Details\n");
        ui_printf("TRIANGLE: Rescan USB\n");
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
        ui_printf("Package files: %u\n",
                  result->installer.source_file_count);
        ui_printf("Package bytes: %llu\n",
                  result->installer.source_total_bytes);
        ui_printf("USB root: %d  close=%d  wait=%u ms\n",
                  result->usb_root_last_dopen,
                  result->usb_root_close_result,
                  result->usb_wait_ms);
        if (!result->package_ready)
            ui_printf("Package error: %s (%d)\n",
                      result->installer.failure_operation,
                      result->installer.failure_return);
        ui_printf("LEFT: Simple view\n");
        ui_printf("TRIANGLE: Rescan USB\n");
    }
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("O Exit\n");
    ui_sync();
}

static void draw_psx1_usb_wait(const psx1_result_t *result)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    ui_inverse_status("CHECKING");
    ui_printf("USB package WAITING\n");
    ui_printf("Elapsed %u / %u ms\n", result->usb_wait_ms,
              USB_WAIT_TIMEOUT_MS);
    ui_printf("Looking for:\n%s\n", INSTALLER_PSX1_SOURCE_ROOT);
    ui_sync();
}

static void wait_for_psx1_usb(psx1_result_t *result)
{
    result->usb_wait_attempted = 1;
    result->usb_ready = 0;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
    result->usb_wait_attempts = 0;
    result->usb_wait_ms = 0;
    for (;;) {
        int directory = fileXioDopen("mass:/");

        result->usb_root_last_dopen = directory;
        ++result->usb_wait_attempts;
        if (directory >= 0) {
            result->usb_root_close_result = fileXioDclose(directory);
            if (result->usb_root_close_result >= 0) {
                result->usb_ready = 1;
                ++result->usb_rescan_count;
                psx1_rescan_package(result);
                break;
            }
        }
        draw_psx1_usb_wait(result);
        if (result->usb_wait_ms >= USB_WAIT_TIMEOUT_MS)
            break;
        DelayThread(USB_POLL_INTERVAL_MS * 1000u);
        result->usb_wait_ms += USB_POLL_INTERVAL_MS;
    }
}

static void draw_psx1_result(const psx1_result_t *result)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    if (result->psx1_installation_valid) {
        ui_inverse_status("PSX1 INSTALLATION COMPLETE");
        ui_inverse_status("FULL POWER OFF REQUIRED");
        ui_printf("Storage and system files: PASS\n");
        ui_printf("Power off the PSX.\n");
        ui_printf("Disconnect AC power before restarting.\n");
        ui_printf("Reconnect and start the PSX normally.\n");
    } else {
        ui_inverse_status("FAILED - STOPPED");
        ui_printf("Step=%d return=%d\n",
                  result->format.failed_step,
                  result->format.failed_result);
        if (result->installer.failure_return != 0) {
            ui_printf("Operation: %s\n",
                      result->installer.failure_operation);
            ui_printf("Path: %s%s\n",
                      result->installer.failure_partition,
                      result->installer.failure_path);
        }
        ui_printf("No later stage was attempted.\n");
    }
    ui_draw_repairbox_logo(408, 168);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit\n");
    ui_sync();
}

static void run_psx1_ui(const pad_diagnostics_t *pad)
{
    static psx1_result_t result;
    struct padButtonStatus buttons;
    unsigned int old_buttons = 0;
    int details = 0;
    int finished = 0;

    (void)pad;
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX1 - First Revision\n\n");
    ui_inverse_status("CHECKING");
    ui_sync();
    psx1_prepare(&result);
    if (!result.package_ready)
        wait_for_psx1_usb(&result);
    draw_psx1_preflight(&result, details);
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~old_buttons;

            old_buttons = current;
            if (!finished && (pressed & PAD_TRIANGLE) != 0) {
                wait_for_psx1_usb(&result);
                details = 0;
                draw_psx1_preflight(&result, details);
            } else if (!finished &&
                (pressed & (PAD_LEFT | PAD_RIGHT)) != 0) {
                details ^= 1;
                draw_psx1_preflight(&result, details);
            } else if (!finished && result.preflight_valid &&
                       confirmation_chord(current, pressed)) {
                psx1_execute(&result);
                finished = 1;
                draw_psx1_result(&result);
            } else if ((pressed & PAD_CIRCLE) != 0 ||
                       (finished && (pressed & PAD_CROSS) != 0)) {
                break;
            }
        }
        DelayThread(16000);
    }
    psx1_release(&result);
}

static void draw_usb_wait(const dr40_result_t *result)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    ui_inverse_status("CHECKING");
    ui_printf("USB package WAITING\n");
    ui_printf("Elapsed %u / %u ms\n", result->usb_wait_ms,
              USB_WAIT_TIMEOUT_MS);
    ui_printf("TRIANGLE: Rescan USB   O: Exit\n");
    ui_sync();
}

static void wait_for_usb_mass(dr40_result_t *result)
{
    result->usb_wait_attempted = 1;
    result->usb_ready = 0;
    result->usb_root_last_dopen = -1;
    result->usb_root_close_result = -1;
    result->usb_wait_attempts = 0;
    result->usb_wait_ms = 0;
    for (;;) {
        int directory = fileXioDopen("mass:/");

        result->usb_root_last_dopen = directory;
        ++result->usb_wait_attempts;
        if (directory >= 0) {
            result->usb_root_close_result = fileXioDclose(directory);
            if (result->usb_root_close_result >= 0) {
                result->usb_ready = 1;
                break;
            }
        }
        draw_usb_wait(result);
        if (result->usb_wait_ms >= USB_WAIT_TIMEOUT_MS)
            break;
        DelayThread(USB_POLL_INTERVAL_MS * 1000u);
        result->usb_wait_ms += USB_POLL_INTERVAL_MS;
    }
}

static void rescan_psx2_preflight(dr40_result_t *result,
                                  storage_diagnostics_t *diagnostics,
                                  capacity_profile_t *profile)
{
    ++result->usb_rescan_count;
    wait_for_usb_mass(result);
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
                                capacity_profile_t profile, int details)
{
    unsigned int index;

    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    if (!details) {
        ui_printf("Hardware       %s\n",
                  result->hardware_profile_valid ? "PASS" : "FAILED");
        ui_printf("Capacity       %s\n", capacity_profile_name(profile));
        ui_printf("Storage        %s\n", psx2_storage_status(result));
        if (result->mode == DR40_MODE_SET_BOUNDARY) {
            ui_printf("System package NOT REQUIRED THIS PASS\n");
            ui_printf("Activation     %s\n",
                      activation->already_armed ? "PRESERVED" : "READY");
        } else {
            ui_printf("System package %s\n",
                      result->source_ready && result->bootstrap_ready
                          ? "READY" : "FAILED");
            ui_printf("Activation     %s\n",
                      activation->already_armed
                          ? "PENDING - PRESERVE"
                          : (activation->generation_valid
                                 ? "READY" : "FAILED"));
        }
        ui_printf("\n");
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
        ui_printf("Details\n\n");
        ui_printf("HDD 0x%08X status=%d fmt=%d\n",
                  (u32)result->before.hdd_totalsector,
                  result->before.hdd_status, result->before.hdd_formatver);
        ui_printf("DVR 0x%08X status=%d fmt=%d\n",
                  (u32)result->before.dvr_totalsector,
                  result->before.dvr_status, result->before.dvr_formatver);
        ui_printf("Native 0x%08X LBA48=%d\n",
                  (u32)result->before.getmaxlba48,
                  result->before.islba48);
        ui_printf("Bootflag: %s\n",
                  result->bootflag.pending_40gb_preserved
                      ? "PENDING_40GB (PRESERVED)"
                      : bootflag_ro_state_name(result->bootflag.state));
        for (index = 0; index < result->installer.partition_count; ++index) {
            const installer_partition_result_t *partition =
                &result->installer.partitions[index];

            if (partition->source_present)
                ui_printf("%-11s files=%u\n", partition->name,
                          partition->source_files);
        }
        ui_printf("LEFT: Simple view   TRIANGLE: Rescan\n");
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
    (void)context;
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    if (stage <= 9)
        ui_inverse_status("PREPARING STORAGE");
    else if (stage <= 11)
        ui_inverse_status("INSTALLING SYSTEM");
    else
        ui_inverse_status("VERIFYING");
    ui_printf("Step %u / %u\n%s\n\n", stage, PROGRAM_STAGE_COUNT, label);
    if (phase == DR40_PROGRESS_BEGIN) {
        ui_inverse_status("IN PROGRESS - DO NOT POWER OFF");
    } else if (phase == DR40_PROGRESS_VALIDATE) {
        ui_inverse_status("VERIFYING");
    } else if (phase == DR40_PROGRESS_COMPLETE) {
        ui_inverse_status("COMPLETE");
    } else {
        ui_inverse_status("FAILED");
    }
    if (phase != DR40_PROGRESS_BEGIN)
        ui_printf("Return=%d  duration=%u ms\n", return_value, duration_ms);
    ui_sync();
    if (phase == DR40_PROGRESS_BEGIN)
        DelayThread(150000);
}

static void draw_activation_progress(dr40_progress_phase_t phase,
                                     int return_value, u32 duration_ms,
                                     const char *operation)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
    ui_inverse_status("ACTIVATING");
    ui_printf("Step %u / %u\n%s\n\n",
              PROGRAM_STAGE_COUNT, PROGRAM_STAGE_COUNT, operation);
    if (phase == DR40_PROGRESS_BEGIN)
        ui_inverse_status("IN PROGRESS - DO NOT POWER OFF");
    else
        ui_inverse_status(phase == DR40_PROGRESS_COMPLETE
                              ? "COMPLETE" : "FAILED");
    if (phase != DR40_PROGRESS_BEGIN)
        ui_printf("Return=%d  duration=%u ms\n", return_value, duration_ms);
    ui_sync();
    if (phase == DR40_PROGRESS_BEGIN)
        DelayThread(150000);
}

static void draw_psx2_result(const dr40_result_t *result,
                             const activation_result_t *activation,
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
        ui_printf("Storage, files and activation: PASS\n");
        if (activation->already_armed)
            ui_printf("Existing XFROM activation preserved.\n");
        ui_printf("Power off the PSX.\n");
        ui_printf("Disconnect AC power before restarting.\n");
        ui_printf("Reconnect and start the PSX normally.\n");
    } else if (result->direct_ready40_storage_valid) {
        ui_inverse_status("ACTIVATION FAILED");
        ui_printf("Storage pipeline: PASS\n");
        ui_printf("XFROM return=%d\n", activation_return);
    } else {
        ui_inverse_status("FAILED - STOPPED");
        ui_printf("Phase=%d return=%d\n", result->failed_phase,
                  result->failure_return);
        ui_printf("Operation: %s\n", result->failure_operation);
        ui_printf("Path: %s%s\n", result->failure_partition,
                  result->failure_path);
    }
    ui_draw_repairbox_logo(408, 168);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit\n");
    ui_sync();
}

static void run_psx2_ui(storage_diagnostics_t *diagnostics,
                        const pad_diagnostics_t *pad)
{
    struct padButtonStatus buttons;
    dr40_result_t result;
    static activation_result_t activation;
    unsigned int old_buttons = 0;
    int finished = 0;
    int details = 0;
    int activation_return = -1;
    capacity_profile_t profile = capacity_profile_detect(
        (u32)diagnostics->dvr_hdd.max_lba48_result,
        diagnostics->dvr_hdd.is_lba48_result);

    (void)pad;
    dr40_initialize(&result, diagnostics);
    activation_initialize(&activation);
    activation_read_current(&activation);
    activation_prepare(&activation);
    bootflag_ro_allow_pending_reinstall(activation.already_armed);
    if (profile == CAPACITY_PROFILE_256_VERIFIED)
        dr40_scan_preflight(&result, diagnostics);
    else
        dr40_multi_scan_preflight(&result, diagnostics);
    if (result.mode == DR40_MODE_INITIALIZE &&
        (!result.source_ready || !result.bootstrap_ready))
        rescan_psx2_preflight(&result, diagnostics, &profile);
    if (result.mode == DR40_MODE_INITIALIZE)
        result.preflight_valid = result.preflight_valid &&
            (activation.generation_valid || activation.already_armed);
    draw_psx2_preflight(&result, &activation, profile, details);
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
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
                if (result.mode == DR40_MODE_INITIALIZE)
                    result.preflight_valid = result.preflight_valid &&
                        (activation.generation_valid ||
                         activation.already_armed);
                details = 0;
                draw_psx2_preflight(&result, &activation, profile, details);
            } else if (!finished &&
                       (pressed & (PAD_LEFT | PAD_RIGHT)) != 0) {
                details ^= 1;
                draw_psx2_preflight(&result, &activation, profile, details);
            } else if (!finished && result.preflight_valid &&
                       confirmation_chord(current, pressed)) {
                result.confirmation_received = 1;
                if (profile == CAPACITY_PROFILE_256_VERIFIED)
                    dr40_execute(&result, diagnostics,
                                 psx2_progress_callback, NULL);
                else
                    dr40_multi_execute(&result, diagnostics,
                                       psx2_progress_callback, NULL);
                if (result.direct_ready40_storage_valid) {
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
                draw_psx2_result(&result, &activation, activation_return,
                                 profile);
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

    (void)argc;
    (void)argv;
    init_scr();
    ui_init();
    initialize_pad(&pad);
    revision = select_revision(&pad);
    if (revision == REVISION_PSX1) {
        run_psx1_ui(&pad);
    } else if (revision == REVISION_PSX2) {
        ui_begin();
        ui_printf(PROGRAM_TITLE "\nPSX2 - Second Revision\n\n");
        ui_inverse_status("CHECKING");
        ui_sync();
        storage_initialize(&psx2_diagnostics);
        storage_refresh_layout_diagnostics(&psx2_diagnostics);
        run_psx2_ui(&psx2_diagnostics, &pad);
        storage_release(&psx2_diagnostics);
    }
    padPortClose(0, 0);
    return 0;
}
