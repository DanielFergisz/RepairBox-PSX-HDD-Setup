#ifndef REPAIRBOX_XMB_SYSTEM_VERSION_H
#define REPAIRBOX_XMB_SYSTEM_VERSION_H

#include "storage.h"

#define PSX_SYSTEM_VERSION_PATH "pfs0:/version.txt"

typedef struct system_version_result {
    int filexio_init_result;
    int pre_umount_result;
    int mount_result;
    int open_result;
    int read_result;
    int close_result;
    int umount_result;
    char text[64];
    psx_revision_t revision;
} system_version_result_t;

int system_version_detect(system_version_result_t *result);

#endif
