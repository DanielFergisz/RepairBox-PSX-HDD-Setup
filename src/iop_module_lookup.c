#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <smem.h>

#include "iop_module_lookup.h"

#define IOP_RAM_BYTES 0x200000u
#define MODULE_LIMIT 256u

/* Fixed-width prefix of PS2SDK smod_mod_info_t, independent of host ABI. */
typedef struct module_prefix {
    uint32_t next;
    uint32_t name;
    uint16_t version;
    uint16_t flags;
    uint16_t id;
    uint16_t unused;
} module_prefix_t;

_Static_assert(sizeof(module_prefix_t) == 16, "IOP module prefix layout");

static int read_iop(uint32_t address, void *output, unsigned int size)
{
    uint32_t region = address & 0xe0000000u;
    uint32_t physical = address & 0x1fffffffu;

    if ((region != 0 && region != 0x80000000u && region != 0xa0000000u) ||
        physical == 0 || physical >= IOP_RAM_BYTES ||
        size > IOP_RAM_BYTES - physical)
        return -EINVAL;
    return smem_read((void *)(uintptr_t)physical, output, size) == size
               ? 0 : -EIO;
}

int iop_module_find(const char *name)
{
    uint32_t address = 0x800u;
    unsigned int count;

    if (name == NULL || name[0] == '\0' || strlen(name) >= 64u)
        return -EINVAL;
    for (count = 0; count < MODULE_LIMIT && address != 0; ++count) {
        module_prefix_t module;
        char module_name[64];

        if ((address & 3u) != 0 ||
            read_iop(address, &module, sizeof(module)) < 0)
            return -EIO;
        if (read_iop(module.name, module_name, sizeof(module_name)) == 0 &&
            memchr(module_name, '\0', sizeof(module_name)) != NULL &&
            strcmp(module_name, name) == 0)
            return module.id;
        address = module.next;
    }
    return address == 0 ? -ENOENT : -ELOOP;
}
