#ifndef REPAIRBOX_XMB_DYNAMIC_APP_INSTALLER_H
#define REPAIRBOX_XMB_DYNAMIC_APP_INSTALLER_H

#include <tamtypes.h>

#include "source_media.h"
#include "storage.h"

#define APP_MAX_COUNT 16
#define APP_MANAGED_PARTITION_MAX 64
#define APP_INSTALLED_FILE_COUNT 6
#define APP_STEP_COUNT 6u
#define APP_USB_ROOT source_media_apps_root()

typedef struct app_package {
    char folder[64];
    char id[48];
    char title[32];
    char subtitle[32];
    char elf_name[64];
    char source_root[192];
    char source_elf[256];
    char source_cover[256];
    char partition_name[32];
    int config_present;
    int cover_present;
} app_package_t;

typedef struct app_preflight {
    app_package_t package;
    int valid;
    int hdd_status;
    int format_version;
    int required_partitions_valid;
    int target_exists;
    u32 target_start;
    u32 target_length;
    int config_result;
    int elf_result;
    u64 elf_size;
    char elf_sha256[65];
    int cover_result;
    u64 cover_size;
    char cover_sha256[65];
    int failure_result;
    char failure_item[96];
} app_preflight_t;

typedef struct app_catalog {
    unsigned int count;
    unsigned int valid_count;
    unsigned int update_count;
    unsigned int new_count;
    int root_open_result;
    int root_close_result;
    int truncated;
    app_preflight_t apps[APP_MAX_COUNT];
} app_catalog_t;

typedef struct app_install_result {
    app_package_t package;
    int success;
    int failed_step;
    int failure_result;
    char failure_operation[64];
    char failure_item[96];
    int target_was_created;
    int target_was_reformatted;
    int target_was_updated;
    u32 target_start;
    u32 target_length;
    unsigned int copied_files;
    unsigned int verified_files;
    u64 copied_bytes;
    char installed_elf_sha256[65];
} app_install_result_t;

typedef struct app_managed_partition {
    char name[32];
    u32 start;
    u32 length;
} app_managed_partition_t;

typedef struct app_uninstall_result {
    int requested;
    int success;
    int scan_result;
    int validation_result;
    int removal_result;
    int verification_result;
    unsigned int candidate_count;
    unsigned int validated_count;
    unsigned int removed_count;
    int failure_result;
    char failure_partition[32];
    app_managed_partition_t partitions[APP_MANAGED_PARTITION_MAX];
} app_uninstall_result_t;

typedef void (*app_progress_callback_t)(unsigned int step,
                                        unsigned int step_count,
                                        const char *operation,
                                        unsigned int percent,
                                        void *context);

void app_scan_catalog(app_catalog_t *catalog);
void app_install(const app_preflight_t *preflight,
                 psx_revision_t revision,
                 app_install_result_t *result,
                 app_progress_callback_t progress,
                 void *context);
int app_scan_managed_partitions(app_uninstall_result_t *result);
void app_uninstall_managed_partitions(app_uninstall_result_t *result);

#endif
