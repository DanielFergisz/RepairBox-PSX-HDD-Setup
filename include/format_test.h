#ifndef REPAIRBOX_PSX1_FORMAT_TEST_H
#define REPAIRBOX_PSX1_FORMAT_TEST_H

#include <tamtypes.h>
#include "inspector.h"

#define FORMAT_TEST_PFS_COUNT 4
#define FORMAT_TEST_CORE_MODULE_COUNT 6
#define FORMAT_TEST_MIN_VISIBLE_SECTORS 0x04A817C8ULL

typedef enum {
    MEDIA_CAPACITY_64_GB = 0,
    MEDIA_CAPACITY_128_GB,
    MEDIA_CAPACITY_256_GB,
    MEDIA_CAPACITY_512_GB,
    MEDIA_CAPACITY_SETMAX_HIDDEN,
    MEDIA_CAPACITY_OTHER
} media_capacity_class_t;

typedef enum {
    PRE_FORMAT_READY_EXISTING_PSX1 = 0,
    PRE_FORMAT_UNFORMATTED,
    PRE_FORMAT_UNKNOWN_PARTITIONING,
    PRE_FORMAT_INVALID_APA,
    PRE_FORMAT_UNSUPPORTED_PHYSICAL_DEVICE,
    PRE_FORMAT_TOO_SMALL,
    PRE_FORMAT_DEVICE_IO_ERROR
} pre_format_state_t;

typedef struct {
    const char *name;
    const char *blockdev;
    int attempted;
    int return_value;
    u32 duration_ms;
    int raw_valid;
} format_stage_t;

typedef struct {
    int preflight_pass;
    pre_format_state_t pre_format_state;
    media_capacity_class_t capacity_class;
    u64 physical_sector_count;
    u64 physical_bytes;
    u32 visible_lba28_sector_count;
    u64 lba48_sector_count;
    int lba48_supported;
    int device_readable;
    int module_usable[FORMAT_TEST_CORE_MODULE_COUNT];
    int usable_module_count;
    int capacity_test_valid;
    int confirmation_received;
    int session_write_locked;
    int storage_activity_stopped;
    int stopped_on_error;
    int failed_step;
    int failed_result;
    format_stage_t apa;
    format_stage_t pfs[FORMAT_TEST_PFS_COUNT];
    int apa_layout_valid;
    int mbr_valid;
    int raw_validation_return;
    u32 raw_validation_duration_ms;
    u32 total_operation_duration_ms;
    int raw_pfs_discovery_valid;
    int psx1_format_test_valid;
} format_test_result_t;

void format_test_init(format_test_result_t *result);
int format_test_preflight(const inspector_data_t *scan);
void format_test_analyze_preflight(const inspector_data_t *scan,
                                   format_test_result_t *result);
const char *format_test_state_name(pre_format_state_t state);
const char *format_test_state_reason(pre_format_state_t state);
const char *format_test_capacity_name(media_capacity_class_t capacity);
int format_test_exact_apa(const inspector_data_t *scan);
int format_test_mbr_valid(const inspector_data_t *scan);
int format_test_strict_pfs_valid(const apa_entry_t *entry);

#endif
