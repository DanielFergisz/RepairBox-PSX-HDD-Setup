#ifndef REPAIRBOX_DIRECT_READY40_H
#define REPAIRBOX_DIRECT_READY40_H

#include <tamtypes.h>

#include "bootflag_ro.h"
#include "bootstrap.h"
#include "installer.h"
#include "storage.h"

#define DR40_VISIBLE_SECTORS 0x04A817C8u
#define DR40_NATIVE_MAX 0x1DD80000u
#define DR40_SYSTEM_COUNT 5u
#define DR40_DVR_COUNT 3u
#define DR40_NORMAL_PFS_COUNT 4u
#define DR40_DVR_PFS_COUNT 2u
#define DR40_STAGE_COUNT 12u

typedef enum dr40_mode {
    DR40_MODE_STOP = 0,
    DR40_MODE_SET_BOUNDARY,
    DR40_MODE_INITIALIZE,
} dr40_mode_t;

typedef struct dr40_operation {
    const char *name;
    int attempted;
    int return_value;
    u32 duration_ms;
    int valid;
} dr40_operation_t;

typedef struct dr40_pfs_result {
    const char *name;
    const char *format_device;
    const char *blockdev;
    const char *mountpoint;
    const char *root_path;
    u32 zone;
    dr40_operation_t format;
    int mount_result;
    int mount_preexisting;
    int mount_owned;
    int zone_size_result;
    int zone_free_result;
    int dopen_result;
    int final_dread_result;
    int dclose_result;
    int umount_attempted;
    int umount_result;
    int filesystem_valid;
} dr40_pfs_result_t;

typedef struct dr40_snapshot {
    int hdd_status;
    int hdd_formatver;
    int hdd_totalsector;
    int hdd_maxsector;
    int hdd_available;
    u32 hdd_entries;
    int dvr_status;
    int dvr_formatver;
    int dvr_totalsector;
    int dvr_maxsector;
    int dvr_available;
    u32 dvr_entries;
    int getmaxlba48;
    int islba48;
} dr40_snapshot_t;

typedef struct dr40_result {
    dr40_mode_t mode;
    int stack_ready;
    int hardware_profile_valid;
    int visible_boundary_valid;
    int bootflag_normal;
    int source_ready;
    int bootstrap_ready;
    int preflight_valid;
    int usb_wait_attempted;
    int usb_ready;
    int usb_root_last_dopen;
    int usb_root_close_result;
    u32 usb_wait_attempts;
    u32 usb_wait_ms;
    u32 usb_rescan_count;
    int confirmation_received;
    int session_write_locked;
    int stopped_on_error;
    int failed_phase;
    char failure_partition[32];
    char failure_path[1024];
    char failure_operation[64];
    int failure_return;
    u32 failure_offset;
    u32 failure_requested_bytes;
    int failure_actual_bytes;
    int power_cycle_required;
    int dvr_mount_cleanup_attempted;
    int dvr_pfs0_cleanup_return;
    int dvr_pfs1_cleanup_return;
    dr40_snapshot_t before;
    dr40_snapshot_t after;
    bootflag_ro_result_t bootflag;
    bootstrap_result_t bootstrap;
    installer_result_t installer;
    dr40_operation_t setmax40;
    dr40_operation_t apa;
    int system_layout_valid;
    dr40_pfs_result_t normal_pfs[DR40_NORMAL_PFS_COUNT];
    dr40_operation_t dvr_hdd;
    int dvr_layout_valid;
    u32 xcontents_end;
    u32 tail_reserved_sectors;
    dr40_pfs_result_t dvr_pfs[DR40_DVR_PFS_COUNT];
    int dvr_directories_valid;
    int all_package_files_valid;
    int final_pfs_valid;
    int final_bootstrap_valid;
    int direct_ready40_storage_valid;
} dr40_result_t;

typedef enum dr40_progress_phase {
    DR40_PROGRESS_BEGIN = 0,
    DR40_PROGRESS_VALIDATE,
    DR40_PROGRESS_COMPLETE,
    DR40_PROGRESS_FAILED,
} dr40_progress_phase_t;

typedef void (*dr40_progress_callback_t)(unsigned int stage,
                                         const char *label,
                                         dr40_progress_phase_t phase,
                                         int return_value,
                                         u32 duration_ms,
                                         void *context);

void dr40_initialize(dr40_result_t *result,
                     const storage_diagnostics_t *diagnostics);
void dr40_scan_preflight(dr40_result_t *result,
                         storage_diagnostics_t *diagnostics);
void dr40_execute(dr40_result_t *result,
                  storage_diagnostics_t *diagnostics,
                  dr40_progress_callback_t progress, void *context);
void dr40_release(dr40_result_t *result);
int dr40_exact_system_layout(const storage_device_result_t *device);
int dr40_exact_dvr_layout(const storage_device_result_t *device,
                          u32 *end, u32 *tail);

#endif
