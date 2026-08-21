#ifndef REPAIRBOX_SYSTEM_FILES_INSTALLER_H
#define REPAIRBOX_SYSTEM_FILES_INSTALLER_H

#include <stddef.h>
#include <tamtypes.h>

#include "storage.h"

#define INSTALLER_PARTITION_COUNT 6
#define INSTALLER_PSX1_PARTITION_COUNT 4
#define INSTALLER_PATH_SIZE 1024
#define INSTALLER_SOURCE_ROOT "mass:/RepairBox-PSX2-SystemFiles"
#define INSTALLER_PSX1_SOURCE_ROOT "mass:/RepairBox-PSX1-SystemFiles"

typedef enum installer_item_type {
    INSTALLER_ITEM_DIRECTORY = 1,
    INSTALLER_ITEM_FILE = 2,
} installer_item_type_t;

typedef struct installer_source_item {
    unsigned int partition_index;
    installer_item_type_t type;
    char *relative_path;
    u64 size;
} installer_source_item_t;

typedef struct installer_partition_result {
    const char *name;
    const char *blockdev;
    const char *mountpoint;
    const char *root_prefix;
    int allow_preexisting_mount;
    int source_present;
    int source_open_result;
    int source_final_read_result;
    int source_close_result;
    u32 source_directories;
    u32 source_files;
    u64 source_bytes;
    int mount_attempted;
    int mount_result;
    int mount_preexisting;
    int mount_owned;
    int mount_probe_result;
    int mount_probe_close_result;
    int umount_attempted;
    int umount_result;
    u32 created_directories;
    u32 copied_files;
    u64 copied_bytes;
    u32 verified_files;
} installer_partition_result_t;

typedef struct installer_result {
    const char *source_root;
    unsigned int partition_count;
    unsigned int progress_step;
    unsigned int progress_step_count;
    const char *progress_operation;
    u32 preflight_failure_mask;
    int source_scan_attempted;
    int source_scan_valid;
    int source_root_open_result;
    int source_root_final_read_result;
    int source_root_close_result;
    u32 source_partition_count;
    u32 source_directory_count;
    u32 source_file_count;
    u64 source_total_bytes;
    u32 current_file_index;
    u64 current_file_size;
    u64 current_file_bytes_processed;
    u64 total_completed_file_bytes;
    u32 copy_elapsed_ms;
    u32 verify_duration_ms;
    u32 unknown_top_level_count;
    char **unknown_top_level_names;
    size_t unknown_top_level_capacity;
    installer_source_item_t *items;
    size_t item_count;
    size_t item_capacity;
    installer_partition_result_t partitions[INSTALLER_PARTITION_COUNT];
    int confirmation_received;
    int install_attempted;
    int session_write_locked;
    int all_unmounted_cleanly;
    int system_files_install_valid;
    int failure_return;
    char failure_partition[32];
    char failure_path[INSTALLER_PATH_SIZE];
    char failure_operation[64];
} installer_result_t;

void installer_initialize(installer_result_t *result,
                          const storage_diagnostics_t *diagnostics);
void installer_initialize_psx1(installer_result_t *result);
void installer_release(installer_result_t *result);
void installer_scan_source(installer_result_t *result);
void installer_execute(installer_result_t *result);
#endif
