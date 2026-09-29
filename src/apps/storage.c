#include <errno.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "apps/storage.h"
#include "source_media.h"

extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[] __attribute__((aligned(16)));
extern unsigned int size_fileXio_irx;
extern unsigned char ps2dev9_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_psx1_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2hdd_psx1_irx;
extern unsigned char ps2fs_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2fs_irx;

typedef struct module_definition {
    const char *name;
    unsigned char *data;
    unsigned int *size;
    unsigned int argument_length;
    const char *arguments;
} module_definition_t;

static const char hdd_arguments[] = "-o\0" "4\0" "-n\0" "20";
static const char pfs_arguments[] =
    "-m\0" "4\0" "-o\0" "10\0" "-n\0" "40";

_Static_assert(sizeof(hdd_arguments) == 11,
               "ps2hdd argument buffer must match wLaunchELF");
_Static_assert(sizeof(pfs_arguments) == 17,
               "ps2fs argument buffer must match wLaunchELF");

static int load_module(storage_result_t *result, unsigned int index,
                       const module_definition_t *module)
{
    int startup = 0x7fffffff;
    int reused_id = source_media_reuse_io_module(module->name);
    if (reused_id >= 0) {
        result->modules[index].name = module->name;
        result->modules[index].module_id = reused_id;
        result->modules[index].startup_result = 0;
        return 0;
    }
    int module_id = SifExecModuleBuffer(
        module->data, *module->size, module->argument_length,
        module->arguments, &startup);

    result->modules[index].name = module->name;
    result->modules[index].module_id = module_id;
    result->modules[index].startup_result = startup;
    return module_id < 0 ? module_id : startup;
}

int storage_initialize_existing_system(storage_result_t *result)
{
    module_definition_t modules[STORAGE_MODULE_COUNT];
    unsigned int index;

    memset(result, 0, sizeof(*result));
    SifInitRpc(0);
    result->lmb_patch_result = sbv_patch_enable_lmb();
    result->prefix_patch_result = sbv_patch_disable_prefix_check();

    modules[0] = (module_definition_t){
        "iomanX", iomanX_irx, &size_iomanX_irx, 0, NULL};
    modules[1] = (module_definition_t){
        "fileXio", fileXio_irx, &size_fileXio_irx, 0, NULL};
    modules[2] = (module_definition_t){
        "ps2dev9", ps2dev9_irx, &size_ps2dev9_irx, 0, NULL};
    modules[3] = (module_definition_t){
        "ps2atad", ps2atad_irx, &size_ps2atad_irx, 0, NULL};
    modules[4] = (module_definition_t){
        "ps2hdd-existing-system", ps2hdd_psx1_irx,
        &size_ps2hdd_psx1_irx, sizeof(hdd_arguments), hdd_arguments};
    modules[5] = (module_definition_t){
        "ps2fs", ps2fs_irx, &size_ps2fs_irx,
        sizeof(pfs_arguments), pfs_arguments};

    for (index = 0; index < 6; ++index) {
        int load_result = load_module(result, index, &modules[index]);

        if (load_result < 0)
            return load_result;
    }
    result->filexio_init_result = fileXioInit();
    if (result->filexio_init_result < 0)
        return result->filexio_init_result;
    /* source_media.c owns the build-specific removable-media stack. */
    for (index = 6; index < STORAGE_MODULE_COUNT; ++index) {
        result->modules[index].name = "source-profile";
        result->modules[index].module_id = 0;
        result->modules[index].startup_result = 0;
    }
    return 0;
}
