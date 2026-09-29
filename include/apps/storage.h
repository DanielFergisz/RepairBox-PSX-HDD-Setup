#ifndef REPAIRBOX_XMB_STORAGE_H
#define REPAIRBOX_XMB_STORAGE_H

#include <tamtypes.h>

typedef enum psx_revision {
    PSX_REVISION_NONE = 0,
    PSX_REVISION_1,
    PSX_REVISION_2
} psx_revision_t;

#define STORAGE_MODULE_COUNT 8

typedef struct storage_module_result {
    const char *name;
    int module_id;
    int startup_result;
} storage_module_result_t;

typedef struct storage_result {
    int lmb_patch_result;
    int prefix_patch_result;
    int filexio_init_result;
    storage_module_result_t modules[STORAGE_MODULE_COUNT];
} storage_result_t;

int storage_initialize_existing_system(storage_result_t *result);

#endif
