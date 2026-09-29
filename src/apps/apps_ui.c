#include <debug.h>
#include <delaythread.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "apps/app_installer.h"
#include "apps/apps_ui.h"
#include "apps/storage.h"
#include "apps/system_version.h"
#include "build_profile.h"
#include "source_media.h"
#include "ui.h"

#define PROGRAM_TITLE RBX_PROGRAM_TITLE
#define USB_WAIT_TIMEOUT_MS 20000u
#define USB_POLL_INTERVAL_MS 250u
#define PROGRESS_REFRESH_MS 500u
#define PROGRESS_LINE_WIDTH 54
#define CATALOG_VISIBLE_ROWS 6u
#define HIDDEN_UNINSTALL_SHOULDERS (PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2)
#define HIDDEN_UNINSTALL_BUTTONS \
    (HIDDEN_UNINSTALL_SHOULDERS | PAD_TRIANGLE)

typedef struct progress_state {
    psx_revision_t revision;
    const char *title;
    unsigned int app_position;
    unsigned int app_count;
    unsigned int last_step;
    unsigned int last_percent;
    u64 start_time;
    u32 last_draw_ms;
    int screen_ready;
} progress_state_t;

static const char *revision_title(psx_revision_t revision)
{
    return revision == PSX_REVISION_1
        ? "PSX1 - First Revision" : "PSX2 - Second Revision";
}

static void draw_storage_loading(void)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n\n");
    ui_inverse_status("LOADING STORAGE MODULES");
    ui_printf("Preparing read-only system detection.\n");
    ui_printf("No disk write has started.\n");
    ui_sync();
}

static void draw_revision_detection(void)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n\n");
    ui_inverse_status("DETECTING PSX SYSTEM REVISION");
    ui_printf("Reading __system/version.txt\n");
    ui_printf("No disk write has started.\n");
    ui_sync();
}

static void draw_detection_failed(const system_version_result_t *version,
                                  int version_result, int storage_result)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n\n");
    if (storage_result < 0) {
        ui_inverse_status("STOP - STORAGE MODULE STARTUP FAILED");
        ui_printf("Storage modules could not start. Result: %d\n",
                  storage_result);
    } else {
        ui_inverse_status("STOP - REVISION DETECTION FAILED");
        ui_printf("Cannot read a supported __system/version.txt.\n");
        ui_printf("Result: %d  mount=%d open=%d read=%d\n",
                  version_result, version->mount_result,
                  version->open_result, version->read_result);
        ui_printf("Detected text: %s\n", version->text);
    }
    ui_printf("No disk write was attempted.\n");
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit");
    ui_sync();
}

static int source_storage_available(void)
{
    return source_media_select(SOURCE_MEDIA_APPS) >= 0;
}

static void wait_for_source(psx_revision_t revision)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    ui_inverse_status("RESCANNING APPLICATION PACKAGE");
    ui_printf("Source: %s\n", source_media_label());
    ui_printf("The launch device remains selected.\n");
    ui_sync();
    (void)source_storage_available();
}

static void draw_loading(psx_revision_t revision, const char *status)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    ui_inverse_status(status);
    ui_printf("No disk write has started.\n");
    ui_sync();
}

static const char *package_action(const app_preflight_t *item)
{
    if (!item->valid)
        return "ERROR";
    return item->target_exists ? "UPDATE" : "NEW";
}

static void draw_catalog(psx_revision_t revision,
                         const system_version_result_t *version,
                         const app_catalog_t *catalog)
{
    unsigned int index;
    unsigned int visible = catalog->count;

    if (visible > CATALOG_VISIBLE_ROWS)
        visible = CATALOG_VISIBLE_ROWS;
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n", revision_title(revision));
    ui_printf("System version: %s (AUTO)\n\n", version->text);
    ui_printf("Source: %s\n", source_media_label());
    for (index = 0; index < visible; ++index) {
        const app_preflight_t *item = &catalog->apps[index];

        ui_printf("%-28.28s %s\n", item->package.title,
                  package_action(item));
    }
    if (catalog->count > visible)
        ui_printf("... and %u more folder(s)\n", catalog->count - visible);
    if (catalog->count == 0)
        ui_printf("No application folders found.\n");
    ui_printf("\nValid: %u  New: %u  Updates: %u\n",
              catalog->valid_count, catalog->new_count,
              catalog->update_count);
    if (catalog->valid_count != 0) {
        ui_inverse_status("READY - VALID APPLICATIONS ONLY");
        if (catalog->valid_count < catalog->count) {
            ui_printf("Invalid packages: %u (skipped).\n",
                      catalog->count - catalog->valid_count);
            ui_printf("Hold L1 + R1 and press X to install.\n");
        } else {
            ui_printf("Hold L1 + R1 and press X to install.\n");
        }
    } else {
        ui_inverse_status("STOP - NO VALID APPLICATION PACKAGE");
        ui_printf("Check folder, ELF and optional app.ini/cover.png.\n");
    }
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("TRIANGLE Rescan     O Exit");
    ui_sync();
}

static u32 progress_elapsed_ms(const progress_state_t *state)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime() - state->start_time,
                      &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static void progress_line(int y, const char *text)
{
    ui_set_position(UI_SAFE_LEFT, y);
    ui_printf("%-*.*s", PROGRESS_LINE_WIDTH, PROGRESS_LINE_WIDTH, text);
}

static void draw_progress(unsigned int step, unsigned int step_count,
                          const char *operation, unsigned int percent,
                          void *context)
{
    progress_state_t *state = context;
    unsigned int work;
    unsigned int overall;
    u32 elapsed = progress_elapsed_ms(state);
    char line[128];

    if (step == state->last_step && percent == state->last_percent)
        return;
    work = state->app_position * APP_STEP_COUNT * 100u +
           (step - 1u) * 100u + percent;
    overall = work / (state->app_count * APP_STEP_COUNT);
    if (state->screen_ready && overall < 100u &&
        elapsed - state->last_draw_ms < PROGRESS_REFRESH_MS)
        return;
    if (!state->screen_ready) {
        ui_begin();
        ui_printf(PROGRAM_TITLE "\n%s\n\n",
                  revision_title(state->revision));
        ui_inverse_status("INSTALLING - DO NOT POWER OFF");
        state->screen_ready = 1;
    }
    snprintf(line, sizeof(line), "Application %u / %u: %s",
             state->app_position + 1u, state->app_count, state->title);
    progress_line(88, line);
    snprintf(line, sizeof(line), "Step %u / %u: %s",
             step, step_count, operation);
    progress_line(112, line);
    snprintf(line, sizeof(line), "Application progress: %u%%", percent);
    progress_line(144, line);
    snprintf(line, sizeof(line), "Overall: %u%%   Elapsed: %02u:%02u",
             overall, elapsed / 60000u, (elapsed / 1000u) % 60u);
    progress_line(168, line);
    ui_sync();
    state->last_step = step;
    state->last_percent = percent;
    state->last_draw_ms = elapsed;
}

static void draw_result(psx_revision_t revision,
                        const app_install_result_t *install,
                        unsigned int install_count,
                        unsigned int completed)
{
    unsigned int index;

    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    if (completed == install_count && install_count != 0) {
        ui_inverse_status("XMB APPLICATION INSTALLATION COMPLETE");
        ui_printf("Installed/updated: %u\n", completed);
        for (index = 0; index < completed && index < 5u; ++index)
            ui_printf("%-28.28s PASS\n", install[index].package.title);
        if (completed > 5u)
            ui_printf("... and %u more application(s)\n", completed - 5u);
        ui_inverse_status("FULL POWER OFF REQUIRED");
        ui_printf("Power off and disconnect AC power.\n");
        ui_printf("Reconnect, start PSX, then open Games.\n");
    } else {
        const app_install_result_t *failure = &install[completed];

        ui_inverse_status("INSTALLATION FAILED - STOPPED");
        ui_printf("Application: %s\n", failure->package.title);
        ui_printf("Step: %d / %u   Error: %d\n",
                  failure->failed_step, APP_STEP_COUNT,
                  failure->failure_result);
        ui_printf("Operation: %s\nItem: %s\n",
                  failure->failure_operation, failure->failure_item);
        ui_printf("Run v1.3 again with the same id to repair/update.\n");
    }
    ui_draw_repairbox_logo(408, 176);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit");
    ui_sync();
}

static int confirmation_chord(unsigned int held, unsigned int pressed)
{
    return (held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
           (pressed & PAD_CROSS) != 0;
}

static int hidden_uninstall_chord(unsigned int held, unsigned int pressed)
{
    return (held & HIDDEN_UNINSTALL_SHOULDERS) ==
               HIDDEN_UNINSTALL_SHOULDERS &&
           (pressed & PAD_TRIANGLE) != 0;
}

static void draw_uninstall_confirmation(
    psx_revision_t revision, const app_uninstall_result_t *uninstall)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    ui_inverse_status("HIDDEN MAINTENANCE MODE");
    if (uninstall->failure_result != 0) {
        ui_printf("Safety scan failed. Nothing was removed.\n");
        ui_printf("Error: %d  Partition: %s\n",
                  uninstall->failure_result,
                  uninstall->failure_partition);
        ui_printf("O Return\n");
    } else if (uninstall->candidate_count == 0) {
        ui_printf("No verified manual XMB applications were found.\n");
        ui_printf("Nothing was removed.\n\nO Return\n");
    } else {
        ui_printf("Verified manual XMB applications: %u\n",
                  uninstall->candidate_count);
        ui_printf("Validated before removal: %u\n\n",
                  uninstall->validated_count);
        ui_printf("This permanently removes every detected manual\n");
        ui_printf("XMB app, including ELF and KELF packages.\n\n");
        ui_inverse_status("CONFIRM PERMANENT DELETE");
        ui_printf("1. Release L1, R1, L2, R2 and TRIANGLE.\n");
        ui_printf("2. Hold [L1]+[R1]+[L2]+[R2], then press\n");
        ui_printf("   [TRIANGLE] again to DELETE ALL.\n\n");
        ui_printf("[O] Cancel\n");
    }
    ui_sync();
}

static int wait_for_uninstall_confirmation(
    psx_revision_t revision, const app_uninstall_result_t *uninstall)
{
    struct padButtonStatus buttons;
    unsigned int previous = 0;
    int released = 0;
    int can_remove = uninstall->failure_result == 0 &&
                     uninstall->candidate_count != 0;

    draw_uninstall_confirmation(revision, uninstall);
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~previous;

            if (!released) {
                if ((current & HIDDEN_UNINSTALL_BUTTONS) == 0)
                    released = 1;
            } else if (can_remove &&
                       hidden_uninstall_chord(current, pressed)) {
                return 1;
            }
            if ((pressed & PAD_CIRCLE) != 0)
                return 0;
            previous = current;
        }
        DelayThread(16000);
    }
}

static void draw_uninstall_removing(psx_revision_t revision)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    ui_inverse_status("REMOVING XMB APPLICATIONS - DO NOT POWER OFF");
    ui_printf("Deleting structurally verified app partitions.\n");
    ui_sync();
}

static void draw_uninstall_result(
    psx_revision_t revision, const app_uninstall_result_t *uninstall)
{
    ui_begin();
    ui_printf(PROGRAM_TITLE "\n%s\n\n", revision_title(revision));
    if (uninstall->success) {
        ui_inverse_status("MANUAL XMB APPLICATIONS REMOVED");
        ui_printf("Removed and verified absent: %u\n",
                  uninstall->removed_count);
        ui_printf("Only validated 128 MiB PFS launch partitions\n");
        ui_printf("with PSX XMB registration were targeted.\n\n");
        ui_inverse_status("NEXT");
        ui_printf("[TRIANGLE] Return to the application installer.\n");
        ui_printf("If finished, power off and disconnect AC power.\n");
    } else {
        ui_inverse_status("UNINSTALL STOPPED");
        ui_printf("Removed before error: %u / %u\n",
                  uninstall->removed_count,
                  uninstall->candidate_count);
        ui_printf("Error: %d  Partition: %s\n",
                  uninstall->failure_result,
                  uninstall->failure_partition);
        ui_printf("Do not retry before checking the error above.\n");
    }
    ui_draw_repairbox_logo(408, 176);
    ui_set_position(UI_SAFE_LEFT, 196);
    ui_printf("X/O Exit");
    ui_sync();
}

static void run_installer(psx_revision_t revision,
                          const system_version_result_t *version)
{
    static app_catalog_t catalog;
    static app_install_result_t install[APP_MAX_COUNT];
    static app_uninstall_result_t uninstall;
    struct padButtonStatus buttons;
    unsigned int previous = 0;
    unsigned int install_count = 0;
    unsigned int completed = 0;
    int finished = 0;
    int uninstall_finished = 0;

    wait_for_source(revision);
    draw_loading(revision, "SCANNING APPLICATION FOLDERS");
    app_scan_catalog(&catalog);
    draw_catalog(revision, version, &catalog);
    for (;;) {
        int state = padGetState(0, 0);

        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(0, 0, &buttons) != 0) {
            unsigned int current = 0xffffu ^ buttons.btns;
            unsigned int pressed = current & ~previous;

            previous = current;
            if (!finished && hidden_uninstall_chord(current, pressed)) {
                draw_loading(revision,
                             "SCANNING MANAGED XMB APPLICATIONS");
                app_scan_managed_partitions(&uninstall);
                if (wait_for_uninstall_confirmation(revision,
                                                    &uninstall)) {
                    previous = 0xffffu;
                    draw_uninstall_removing(revision);
                    app_uninstall_managed_partitions(&uninstall);
                    finished = 1;
                    uninstall_finished = 1;
                    install_count = 0;
                    completed = 0;
                    draw_uninstall_result(revision, &uninstall);
                } else {
                    previous = 0xffffu;
                    memset(&uninstall, 0, sizeof(uninstall));
                    draw_catalog(revision, version, &catalog);
                }
            } else if (!finished && catalog.valid_count != 0 &&
                confirmation_chord(current, pressed)) {
                unsigned int index;
                progress_state_t progress;

                memset(install, 0, sizeof(install));
                memset(&uninstall, 0, sizeof(uninstall));
                memset(&progress, 0, sizeof(progress));
                progress.revision = revision;
                progress.app_count = catalog.valid_count;
                progress.last_percent = 101u;
                progress.start_time = GetTimerSystemTime();
                install_count = catalog.valid_count;
                for (index = 0; index < catalog.count; ++index) {
                    if (!catalog.apps[index].valid)
                        continue;
                    progress.app_position = completed;
                    progress.title = catalog.apps[index].package.title;
                    app_install(&catalog.apps[index], revision,
                                &install[completed], draw_progress,
                                &progress);
                    if (!install[completed].success)
                        break;
                    ++completed;
                }
                finished = 1;
                uninstall_finished = 0;
                draw_result(revision, install, install_count, completed);
            } else if (!finished && (pressed & PAD_TRIANGLE) != 0) {
                draw_loading(revision, "RESCANNING APPLICATIONS");
                wait_for_source(revision);
                app_scan_catalog(&catalog);
                draw_catalog(revision, version, &catalog);
            } else if (uninstall_finished && uninstall.success &&
                       (pressed & PAD_TRIANGLE) != 0) {
                draw_loading(revision, "RESCANNING APPLICATIONS");
                wait_for_source(revision);
                memset(install, 0, sizeof(install));
                memset(&uninstall, 0, sizeof(uninstall));
                install_count = 0;
                completed = 0;
                finished = 0;
                uninstall_finished = 0;
                app_scan_catalog(&catalog);
                draw_catalog(revision, version, &catalog);
            } else if ((pressed & PAD_CIRCLE) != 0 ||
                       (finished && (pressed & PAD_CROSS) != 0)) {
                return;
            }
        }
        DelayThread(16000);
    }
}

void apps_ui_run(void)
{
    static system_version_result_t version;
    static storage_result_t storage;
    struct padButtonStatus buttons;
    unsigned int previous = 0;
    int storage_result;
    int version_result;
    static char source_pad_buffer[256] __attribute__((aligned(64)));

    draw_storage_loading();
    storage_result = storage_initialize_existing_system(&storage);
    if (storage_result >= 0) {
        if (source_media_needs_optional_modules()) {
            if (!source_media_is_mmce())
                (void)padPortClose(0, 0);
            DelayThread(100000);
            (void)source_media_load_optional_modules();
            if (!source_media_is_mmce())
                (void)padPortOpen(0, 0, source_pad_buffer);
            {
                unsigned int attempts;

                for (attempts = 0; attempts < 300u; ++attempts) {
                    int state = padGetState(0, 0);

                    if (state == PAD_STATE_STABLE ||
                        state == PAD_STATE_FINDCTP1)
                        break;
                    DelayThread(10000);
                }
            }
        }
        (void)source_media_select(SOURCE_MEDIA_APPS);
    }
    version_result = storage_result;
    if (storage_result >= 0) {
        draw_revision_detection();
        version_result = system_version_detect(&version);
    }
    if (version_result >= 0 && version.revision != PSX_REVISION_NONE) {
        run_installer(version.revision, &version);
    } else {
        draw_detection_failed(&version, version_result, storage_result);
        for (;;) {
            int state = padGetState(0, 0);
            if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
                padRead(0, 0, &buttons) != 0) {
                unsigned int current = 0xffffu ^ buttons.btns;
                unsigned int pressed = current & ~previous;
                previous = current;
                if ((pressed & (PAD_CROSS | PAD_CIRCLE)) != 0) {
                    break;
                }
            }
            DelayThread(16000);
        }
    }
}
