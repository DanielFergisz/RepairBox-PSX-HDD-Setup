#ifndef REPAIRBOX_PSX1_PIPELINE_H
#define REPAIRBOX_PSX1_PIPELINE_H

#include "format_test.h"
#include "installer.h"

typedef struct psx1_result {
    inspector_data_t before;
    inspector_data_t formatted;
    inspector_data_t final_layout;
    format_test_result_t format;
    installer_result_t installer;
    int package_ready;
    int preflight_valid;
    int hardware_verified_profile;
    int usb_wait_attempted;
    int usb_ready;
    int usb_root_last_dopen;
    int usb_root_close_result;
    unsigned int usb_wait_attempts;
    unsigned int usb_wait_ms;
    unsigned int usb_rescan_count;
    int final_apa_valid;
    int final_mbr_valid;
    int final_pfs_valid[FORMAT_TEST_PFS_COUNT];
    int final_raw_pfs_valid;
    int all_package_files_valid;
    int psx1_storage_valid;
    int psx1_installation_valid;
} psx1_result_t;

void psx1_prepare(psx1_result_t *result);
const char *psx1_capacity_name(media_capacity_class_t capacity);
void psx1_rescan_package(psx1_result_t *result);
void psx1_execute(psx1_result_t *result);
void psx1_release(psx1_result_t *result);

#endif
