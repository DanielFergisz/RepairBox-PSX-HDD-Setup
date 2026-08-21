#include <delaythread.h>
#include <errno.h>
#include <hdd-ioctl.h>
#include <libcdvd.h>
#include <libmc.h>
#include <malloc.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <string.h>

#include "storage.h"

extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[] __attribute__((aligned(16)));
extern unsigned int size_fileXio_irx;
extern unsigned char ps2dev9_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2hdd_irx;
extern unsigned char ps2fs_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2fs_irx;
extern unsigned char dvrdrv_irx[] __attribute__((aligned(16)));
extern unsigned int size_dvrdrv_irx;
extern unsigned char dvrfile_irx[] __attribute__((aligned(16)));
extern unsigned int size_dvrfile_irx;
extern unsigned char usbd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbhdfsd_irx;
extern unsigned char extflash_irx[] __attribute__((aligned(16)));
extern unsigned int size_extflash_irx;
extern unsigned char xfromman_irx[] __attribute__((aligned(16)));
extern unsigned int size_xfromman_irx;
extern unsigned char xfromserv_irx[] __attribute__((aligned(16)));
extern unsigned int size_xfromserv_irx;

extern int xfromInit(int type);

typedef struct embedded_module {
    const char *name;
    unsigned char *data;
    unsigned int *size;
    unsigned int argument_length;
    const char *arguments;
} embedded_module_t;

enum storage_module_index {
    MODULE_IOMANX = 0,
    MODULE_FILEXIO,
    MODULE_PS2DEV9,
    MODULE_PS2ATAD,
    MODULE_PS2HDD,
    MODULE_PS2FS,
    MODULE_DVRDRV,
    MODULE_DVRFILE,
    MODULE_USBD,
    MODULE_USBHDFSD,
    MODULE_EXTFLASH,
    MODULE_XFROMMAN,
    MODULE_XFROMSERV,
};

#define DVRP_NOTICE_DELAY_US 500000
#define INITIAL_ENTRY_CAPACITY 16

static const char ps2hdd_arguments[] = "-o\0" "4\0" "-n\0" "20";
static const char ps2fs_arguments[] =
    "-m\0" "4\0" "-o\0" "10\0" "-n\0" "40";

_Static_assert(sizeof(ps2hdd_arguments) == 11,
               "ps2hdd argument buffer must match wLaunchELF");
_Static_assert(sizeof(ps2fs_arguments) == 17,
               "ps2fs argument buffer must match wLaunchELF");

static const embedded_module_t embedded_modules[STORAGE_MODULE_COUNT] = {
    {"iomanX", iomanX_irx, &size_iomanX_irx, 0, NULL},
    {"fileXio", fileXio_irx, &size_fileXio_irx, 0, NULL},
    {"ps2dev9", ps2dev9_irx, &size_ps2dev9_irx, 0, NULL},
    {"ps2atad", ps2atad_irx, &size_ps2atad_irx, 0, NULL},
    {"ps2hdd", ps2hdd_irx, &size_ps2hdd_irx,
     sizeof(ps2hdd_arguments), ps2hdd_arguments},
    {"ps2fs", ps2fs_irx, &size_ps2fs_irx,
     sizeof(ps2fs_arguments), ps2fs_arguments},
    {"dvrdrv", dvrdrv_irx, &size_dvrdrv_irx, 0, NULL},
    {"dvrfile", dvrfile_irx, &size_dvrfile_irx, 0, NULL},
    {"usbd", usbd_irx, &size_usbd_irx, 0, NULL},
    {"usbhdfsd", usbhdfsd_irx, &size_usbhdfsd_irx, 0, NULL},
    {"extflash", extflash_irx, &size_extflash_irx, 0, NULL},
    {"xfromman", xfromman_irx, &size_xfromman_irx, 0, NULL},
    {"xfromserv", xfromserv_irx, &size_xfromserv_irx, 0, NULL},
};

static void set_device_defaults(storage_device_result_t *device,
                                const char *name)
{
    memset(device, 0, sizeof(*device));
    device->name = name;
    device->dopen_result = -1;
    device->final_dread_result = -1;
    device->dclose_result = -1;
    device->status_result = -1;
    device->format_version_result = -1;
    device->total_sectors_result = -1;
    device->max_partition_sectors_result = -1;
    device->max_lba48_result = -1;
    device->is_lba48_result = -1;
}

static void clear_device_scan(storage_device_result_t *device)
{
    const char *name = device->name;

    free(device->entries);
    set_device_defaults(device, name);
}

static void load_embedded_module(storage_diagnostics_t *diagnostics,
                                 int index)
{
    const embedded_module_t *module = &embedded_modules[index];
    int module_result = 0x7fffffff;

    diagnostics->modules[index].name = module->name;
    diagnostics->modules[index].module_id =
        SifExecModuleBuffer(module->data, *module->size,
                            module->argument_length, module->arguments,
                            &module_result);
    diagnostics->modules[index].module_result = module_result;
}

static u32 timer_delta_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

void storage_initialize(storage_diagnostics_t *diagnostics)
{
    u64 dvrfile_start;
    u64 dvrfile_end;
    int index;

    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->xfrom_init_result = -ENODEV;
    set_device_defaults(&diagnostics->hdd, "hdd0:");
    set_device_defaults(&diagnostics->dvr_hdd, "dvr_hdd0:");

    /* Preserve the launcher's IOP and the working PSX DVRP state. */
    SifInitRpc(0);
    diagnostics->lmb_patch_result = sbv_patch_enable_lmb();
    diagnostics->prefix_patch_result = sbv_patch_disable_prefix_check();

    for (index = MODULE_IOMANX; index <= MODULE_PS2FS; ++index)
        load_embedded_module(diagnostics, index);

    diagnostics->cdvd_init_result = sceCdInit(SCECdINoD);
    diagnostics->notice_result = 0;
    diagnostics->notice_game_start_result =
        sceCdNoticeGameStart(0, &diagnostics->notice_result);
    DelayThread(DVRP_NOTICE_DELAY_US);

    load_embedded_module(diagnostics, MODULE_DVRDRV);
    dvrfile_start = GetTimerSystemTime();
    load_embedded_module(diagnostics, MODULE_DVRFILE);
    dvrfile_end = GetTimerSystemTime();
    diagnostics->dvrfile_load_time_ms =
        timer_delta_ms(dvrfile_start, dvrfile_end);

    diagnostics->filexio_init_result = fileXioInit();
    load_embedded_module(diagnostics, MODULE_USBD);
    load_embedded_module(diagnostics, MODULE_USBHDFSD);
    load_embedded_module(diagnostics, MODULE_EXTFLASH);
    load_embedded_module(diagnostics, MODULE_XFROMMAN);
    load_embedded_module(diagnostics, MODULE_XFROMSERV);
    if (diagnostics->modules[MODULE_XFROMSERV].module_id >= 0 &&
        diagnostics->modules[MODULE_XFROMSERV].module_result == 0)
        diagnostics->xfrom_init_result = xfromInit(MC_TYPE_MC);
}

static int append_entry(storage_device_result_t *device,
                        const iox_dirent_t *source)
{
    storage_entry_result_t *entry;

    if (device->entry_count == device->entry_capacity) {
        unsigned int capacity = device->entry_capacity == 0
                                    ? INITIAL_ENTRY_CAPACITY
                                    : device->entry_capacity * 2;
        storage_entry_result_t *entries =
            realloc(device->entries, capacity * sizeof(*entries));

        if (entries == NULL) {
            device->entry_allocation_failed = 1;
            return -1;
        }
        device->entries = entries;
        device->entry_capacity = capacity;
    }

    entry = &device->entries[device->entry_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, source->name, sizeof(entry->name));
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->mode = source->stat.mode;
    entry->attr = source->stat.attr;
    entry->size = source->stat.size;
    entry->hisize = source->stat.hisize;
    entry->private_fields[0] = source->stat.private_0;
    entry->private_fields[1] = source->stat.private_1;
    entry->private_fields[2] = source->stat.private_2;
    entry->private_fields[3] = source->stat.private_3;
    entry->private_fields[4] = source->stat.private_4;
    entry->private_fields[5] = source->stat.private_5;
    return 0;
}

static void scan_root(storage_device_result_t *device)
{
    iox_dirent_t entry;
    int fd = fileXioDopen(device->name);
    int read_result;

    device->dopen_result = fd;
    if (fd < 0) {
        device->available = 0;
        device->final_dread_result = fd;
        return;
    }

    device->available = 1;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        read_result = fileXioDread(fd, &entry);
        if (read_result <= 0)
            break;
        if (append_entry(device, &entry) < 0)
            break;
    }
    device->final_dread_result = read_result;
    device->dclose_result = fileXioDclose(fd);
}

static void query_common_values(storage_device_result_t *device)
{
    device->status_result =
        fileXioDevctl(device->name, HDIOC_STATUS, NULL, 0, NULL, 0);
    device->format_version_result =
        fileXioDevctl(device->name, HDIOC_FORMATVER, NULL, 0, NULL, 0);
    device->total_sectors_result =
        fileXioDevctl(device->name, HDIOC_TOTALSECTOR, NULL, 0, NULL, 0);
    device->max_partition_sectors_result =
        fileXioDevctl(device->name, HDIOC_MAXSECTOR, NULL, 0, NULL, 0);
}

void storage_refresh_layout_diagnostics(storage_diagnostics_t *diagnostics)
{
    clear_device_scan(&diagnostics->hdd);
    clear_device_scan(&diagnostics->dvr_hdd);

    query_common_values(&diagnostics->hdd);
    query_common_values(&diagnostics->dvr_hdd);
    diagnostics->dvr_hdd.max_lba48_result =
        fileXioDevctl("dvr_hdd0:", HDIOC_GETMAXLBA48,
                     NULL, 0, NULL, 0);
    diagnostics->dvr_hdd.is_lba48_result =
        fileXioDevctl("dvr_hdd0:", HDIOC_ISLBA48,
                     NULL, 0, NULL, 0);

    scan_root(&diagnostics->hdd);
    scan_root(&diagnostics->dvr_hdd);
}

void storage_release(storage_diagnostics_t *diagnostics)
{
    free(diagnostics->hdd.entries);
    diagnostics->hdd.entries = NULL;
    diagnostics->hdd.entry_count = 0;
    diagnostics->hdd.entry_capacity = 0;
    free(diagnostics->dvr_hdd.entries);
    diagnostics->dvr_hdd.entries = NULL;
    diagnostics->dvr_hdd.entry_count = 0;
    diagnostics->dvr_hdd.entry_capacity = 0;
}

const char *storage_result_name(int result)
{
    if (result >= 0)
        return "OK";
    switch (-result) {
        case ENOENT:
            return "ENOENT";
        case EIO:
            return "EIO";
        case ENXIO:
            return "ENXIO";
        case EBUSY:
            return "EBUSY";
        case ENODEV:
            return "ENODEV";
        case EINVAL:
            return "EINVAL";
        case ENOSYS:
            return "ENOSYS";
        case EPERM:
            return "EPERM";
        default:
            return "ERROR";
    }
}
