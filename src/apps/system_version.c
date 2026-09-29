#include <errno.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "apps/system_version.h"

int system_version_detect(system_version_result_t *result)
{
    int fd;

    memset(result, 0, sizeof(*result));
    result->revision = PSX_REVISION_NONE;
    /* storage_initialize_existing_system() owns fileXio startup.  Calling
       fileXioInit() here used services inherited from the launcher in RC3
       and could block before the installer's HDD/PFS modules were loaded. */
    result->filexio_init_result = 0;

    result->pre_umount_result = fileXioUmount("pfs0:");
    result->mount_result = fileXioMount(
        "pfs0:", "hdd0:__system", FIO_MT_RDONLY);
    if (result->mount_result < 0)
        return result->mount_result;

    fd = fileXioOpen(PSX_SYSTEM_VERSION_PATH, FIO_O_RDONLY, 0);
    result->open_result = fd;
    if (fd < 0) {
        result->umount_result = fileXioUmount("pfs0:");
        return fd;
    }
    result->read_result = fileXioRead(
        fd, result->text, sizeof(result->text) - 1u);
    result->close_result = fileXioClose(fd);
    result->umount_result = fileXioUmount("pfs0:");
    if (result->read_result < 0)
        return result->read_result;
    if (result->close_result < 0)
        return result->close_result;
    if (result->umount_result < 0)
        return result->umount_result;
    if (result->read_result == 0)
        return -EINVAL;

    result->text[result->read_result] = '\0';
    if (result->text[0] == '1')
        result->revision = PSX_REVISION_1;
    else if (result->text[0] == '2')
        result->revision = PSX_REVISION_2;
    else
        return -EINVAL;
    return 0;
}
