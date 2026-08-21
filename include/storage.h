#ifndef REPAIRBOX_SYSTEM_FILES_DUMPER_STORAGE_H
#define REPAIRBOX_SYSTEM_FILES_DUMPER_STORAGE_H

#include <tamtypes.h>

#define STORAGE_MODULE_COUNT 13

typedef struct storage_module_result {
    const char *name;
    int module_id;
    int module_result;
} storage_module_result_t;

typedef struct storage_entry_result {
    char name[256];
    u32 mode;
    u32 attr;
    u32 size;
    u32 hisize;
    u32 private_fields[6];
} storage_entry_result_t;

typedef struct storage_device_result {
    const char *name;
    int available;
    int dopen_result;
    int final_dread_result;
    int dclose_result;
    unsigned int entry_count;
    unsigned int entry_capacity;
    int entry_allocation_failed;
    storage_entry_result_t *entries;
    int status_result;
    int format_version_result;
    int total_sectors_result;
    int max_partition_sectors_result;
    int max_lba48_result;
    int is_lba48_result;
} storage_device_result_t;

typedef struct storage_diagnostics {
    int lmb_patch_result;
    int prefix_patch_result;
    int filexio_init_result;
    int cdvd_init_result;
    int notice_game_start_result;
    u32 notice_result;
    u32 dvrfile_load_time_ms;
    int xfrom_init_result;
    storage_module_result_t modules[STORAGE_MODULE_COUNT];
    storage_device_result_t hdd;
    storage_device_result_t dvr_hdd;
} storage_diagnostics_t;

void storage_initialize(storage_diagnostics_t *diagnostics);
void storage_refresh_layout_diagnostics(storage_diagnostics_t *diagnostics);
void storage_release(storage_diagnostics_t *diagnostics);
const char *storage_result_name(int result);

#endif
