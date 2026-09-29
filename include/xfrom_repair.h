#ifndef REPAIRBOX_XFROM_REPAIR_H
#define REPAIRBOX_XFROM_REPAIR_H

#include <tamtypes.h>

#define XFROM_REPAIR_MAX_FILES 64u
#define XFROM_REPAIR_PATH_SIZE 192u

typedef enum xfrom_repair_revision {
    XFROM_REPAIR_PSX1 = 1,
    XFROM_REPAIR_PSX2 = 2
} xfrom_repair_revision_t;

typedef enum xfrom_progress_phase {
    XFROM_PROGRESS_VALIDATE_BACKUP = 0,
    XFROM_PROGRESS_PREPARE_TARGET,
    XFROM_PROGRESS_SCAN_CLEANUP,
    XFROM_PROGRESS_REMOVE_EXTRAS,
    XFROM_PROGRESS_VERIFY_CLEANUP,
    XFROM_PROGRESS_RELOAD_BACKUP,
    XFROM_PROGRESS_WRITE_TARGET,
    XFROM_PROGRESS_VERIFY_TARGET,
} xfrom_progress_phase_t;

typedef void (*xfrom_progress_callback_t)(
    xfrom_progress_phase_t phase, const char *path,
    u32 file_index, u32 file_count, u32 completed, u32 total,
    void *context);

typedef struct xfrom_repair_entry {
    char path[XFROM_REPAIR_PATH_SIZE];
    u8 expected_sha256[32];
    u32 size;
    int source_nested;
    int deferred_activation;
    int source_valid;
    int destination_valid;
    int write_attempted;
} xfrom_repair_entry_t;

typedef struct xfrom_repair_result {
    xfrom_repair_entry_t files[XFROM_REPAIR_MAX_FILES];
    u32 file_count;
    u32 total_bytes;
    u32 files_written;
    u32 files_preserved;
    u32 files_deferred;
    xfrom_repair_revision_t revision;
    int core_files_valid;
    int source_valid;
    int xfrom_root_open_result;
    int xfrom_root_close_result;
    int xfrom_root_accessible;
    int xfrom_partition_open_result;
    int xfrom_partition_close_result;
    int xfrom_partition_accessible;
    int xfrom_partition_missing;
    int xfrom_partition_created;
    u32 cleanup_entries_scanned;
    u32 cleanup_candidates;
    u32 cleanup_removed;
    int cleanup_valid;
    int write_attempted;
    int repair_valid;
    int failure_return;
    char failure_operation[48];
    char failure_path[XFROM_REPAIR_PATH_SIZE];
    u32 duration_ms;
} xfrom_repair_result_t;

void xfrom_repair_initialize(xfrom_repair_result_t *result);
void xfrom_repair_validate_source(xfrom_repair_result_t *result,
                                  xfrom_repair_revision_t revision,
                                  xfrom_progress_callback_t progress,
                                  void *progress_context);
int xfrom_repair_execute(xfrom_repair_result_t *result,
                         int storage_valid, int session_confirmed,
                         xfrom_progress_callback_t progress,
                         void *progress_context);

#endif
