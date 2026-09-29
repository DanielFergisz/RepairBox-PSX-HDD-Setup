#include <delaythread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "build_profile.h"
#include "source_media.h"
#include "iop_module_lookup.h"

#define SOURCE_COUNT 4u
#define MMCE_READ_SIZE (32u * 1024u)
#define SEARCH_MAX_DEPTH 2u
#define SEARCH_MAX_DIRECTORIES 48u
#define SEARCH_MAX_ENTRIES 384u
#define PATH_BUFFER_SIZE 384u

#if !RBX_BUILD_MMCE
extern unsigned char bdm_irx[] __attribute__((aligned(16)));
extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[] __attribute__((aligned(16)));
extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char mx4sio_bd_irx[] __attribute__((aligned(16)));
extern unsigned int size_mx4sio_bd_irx;
extern unsigned char usbd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbd_irx;
extern unsigned char usbmass_bd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbmass_bd_irx;
#else
extern unsigned char mmceman_irx[] __attribute__((aligned(16)));
extern unsigned int size_mmceman_irx;
#endif
extern unsigned char sio2man_irx[] __attribute__((aligned(16)));
extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[] __attribute__((aligned(16)));
extern unsigned int size_padman_irx;
extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[] __attribute__((aligned(16)));
extern unsigned int size_fileXio_irx;

typedef struct source_definition {
    const char *label;
    const char *device;
} source_definition_t;

typedef struct search_directory {
    char relative[192];
    unsigned int depth;
} search_directory_t;

static const source_definition_t sources[SOURCE_COUNT] = {
    {"USB", "usb:"},
    {"MX4SIO", "mx4sio:"},
    {"MMCE 1", "mmce0:"},
    {"MMCE 2", "mmce1:"},
};

static int preferred_source = -1;
static int active_source = -1;
static int rejected_source = -1;
static int unresolved_source_active;
static int optional_modules_loaded;
static int optional_modules_result;
static int resolution_result = -ENODEV;
static int media_ready;
static char launch_file[96];
static char launch_directory[192];
static char package_base[192];
static char psx1_root[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX1-SystemFiles";
static char psx2_root[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX2-SystemFiles";
static char bootstrap_root[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX2-Bootstrap";
static char bootstrap_bin[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX2-Bootstrap/mbr_bootstrap_prefix.bin";
static char bootstrap_sums[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX2-Bootstrap/SHA256SUMS.txt";
static char psx1_xfrom_root[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX1-XFROM";
static char psx2_xfrom_root[PATH_BUFFER_SIZE] =
    "invalid:/RepairBox-PSX2-XFROM";
static char apps_root[PATH_BUFFER_SIZE] = "invalid:/PSX_XMB_Apps";

static void set_active_source(unsigned int index);

static int source_index_allowed(unsigned int index)
{
#if RBX_BUILD_MMCE
    return index == 2u || index == 3u;
#else
    return index == 0u || index == 1u;
#endif
}

static int copy_text(char *output, size_t output_size, const char *text)
{
    size_t length = strlen(text);

    if (length + 1u > output_size)
        return 0;
    memcpy(output, text, length + 1u);
    return 1;
}

static int append_text(char *output, size_t output_size, const char *text)
{
    size_t used = strlen(output);
    size_t length = strlen(text);

    if (used + length + 1u > output_size)
        return 0;
    memcpy(output + used, text, length + 1u);
    return 1;
}

static int device_token_at(const char *text, const char *token,
                           size_t token_length)
{
    const char *position;

    if (text == NULL)
        return 0;
    for (position = text; *position != '\0'; ++position) {
        const char *end;

        if (strncasecmp(position, token, token_length) != 0)
            continue;
        end = position + token_length;
        while (*end >= '0' && *end <= '9')
            ++end;
        if (*end == ':')
            return 1;
    }
    return 0;
}

static int identify_source(const char *path)
{
    if (device_token_at(path, "usb", sizeof("usb") - 1))
        return 0;
    if (device_token_at(path, "mx4sio", sizeof("mx4sio") - 1))
        return 1;
    if (device_token_at(path, "mmce0", 5))
        return 2;
    if (device_token_at(path, "mmce1", 5))
        return 3;
    return -1;
}

static void remember_launch_path(const char *path)
{
    char normalized[PATH_BUFFER_SIZE];
    const char *relative;
    char *last_slash;
    size_t index;

    if (path == NULL || path[0] == '\0')
        return;
    snprintf(normalized, sizeof(normalized), "%s", path);
    for (index = 0; normalized[index] != '\0'; ++index) {
        if (normalized[index] == '\\')
            normalized[index] = '/';
    }
    relative = strchr(normalized, ':');
    relative = relative != NULL ? relative + 1 : normalized;
    while (*relative == '/')
        ++relative;
    if (!copy_text(launch_file, sizeof(launch_file), relative)) {
        launch_file[0] = '\0';
        launch_directory[0] = '\0';
        return;
    }
    last_slash = strrchr(launch_file, '/');
    if (last_slash == NULL) {
        launch_directory[0] = '\0';
        return;
    }
    *last_slash = '\0';
    if (!copy_text(launch_directory, sizeof(launch_directory), launch_file) ||
        !copy_text(launch_file, sizeof(launch_file), last_slash + 1)) {
        launch_file[0] = '\0';
        launch_directory[0] = '\0';
    }
}

static void build_path(char *output, size_t output_size,
                       const char *device, const char *base,
                       const char *leaf)
{
    output[0] = '\0';
    if (!copy_text(output, output_size, device) ||
        !append_text(output, output_size, "/") ||
        (base != NULL && base[0] != '\0' &&
         (!append_text(output, output_size, base) ||
          (leaf[0] != '\0' && !append_text(output, output_size, "/")))) ||
        !append_text(output, output_size, leaf))
        copy_text(output, output_size, "invalid:/path-too-long");
}

static int build_relative(char *output, size_t output_size,
                          const char *base, const char *name)
{
    output[0] = '\0';
    return copy_text(output, output_size, base) &&
           (base[0] == '\0' || append_text(output, output_size, "/")) &&
           append_text(output, output_size, name);
}

static int directory_readable(const char *path)
{
    int fd = fileXioDopen(path);
    int close_result;

    if (fd < 0)
        return 0;
    close_result = fileXioDclose(fd);
    return close_result >= 0;
}

static int base_has_content(unsigned int source,
                            source_media_content_t content,
                            const char *base)
{
    char first[PATH_BUFFER_SIZE];
    char second[PATH_BUFFER_SIZE];
    char third[PATH_BUFFER_SIZE];
    const char *device = sources[source].device;

    if (content == SOURCE_MEDIA_PSX1_SYSTEM) {
        build_path(first, sizeof(first), device, base,
                   "RepairBox-PSX1-SystemFiles");
        build_path(second, sizeof(second), device, base,
                   "RepairBox-PSX1-XFROM");
        return directory_readable(first) && directory_readable(second);
    }
    if (content == SOURCE_MEDIA_APPS) {
        build_path(first, sizeof(first), device, base, "PSX_XMB_Apps");
        return directory_readable(first);
    }
    build_path(first, sizeof(first), device, base,
               "RepairBox-PSX2-SystemFiles");
    build_path(second, sizeof(second), device, base,
               "RepairBox-PSX2-Bootstrap");
    build_path(third, sizeof(third), device, base,
               "RepairBox-PSX2-XFROM");
    return directory_readable(first) && directory_readable(second) &&
           directory_readable(third);
}

static void apply_package_base(unsigned int source, const char *base)
{
    const char *device = sources[source].device;

    if (!copy_text(package_base, sizeof(package_base),
                   base != NULL ? base : ""))
        package_base[0] = '\0';
    build_path(psx1_root, sizeof(psx1_root), device, package_base,
               "RepairBox-PSX1-SystemFiles");
    build_path(psx2_root, sizeof(psx2_root), device, package_base,
               "RepairBox-PSX2-SystemFiles");
    build_path(bootstrap_root, sizeof(bootstrap_root), device, package_base,
               "RepairBox-PSX2-Bootstrap");
    build_path(bootstrap_bin, sizeof(bootstrap_bin), device, package_base,
               "RepairBox-PSX2-Bootstrap/mbr_bootstrap_prefix.bin");
    build_path(bootstrap_sums, sizeof(bootstrap_sums), device, package_base,
               "RepairBox-PSX2-Bootstrap/SHA256SUMS.txt");
    build_path(psx1_xfrom_root, sizeof(psx1_xfrom_root), device,
               package_base, "RepairBox-PSX1-XFROM");
    build_path(psx2_xfrom_root, sizeof(psx2_xfrom_root), device, package_base,
               "RepairBox-PSX2-XFROM");
    build_path(apps_root, sizeof(apps_root), device, package_base,
               "PSX_XMB_Apps");
}

static int try_launch_ancestors(unsigned int source,
                                source_media_content_t content,
                                char *base, size_t base_size)
{
    char candidate[192];

    copy_text(candidate, sizeof(candidate), launch_directory);
    for (;;) {
        char *slash;

        if (base_has_content(source, content, candidate)) {
            copy_text(base, base_size, candidate);
            return 1;
        }
        if (candidate[0] == '\0')
            break;
        slash = strrchr(candidate, '/');
        if (slash == NULL)
            candidate[0] = '\0';
        else
            *slash = '\0';
    }
    return 0;
}

static int queue_contains(const search_directory_t *queue,
                          unsigned int count, const char *relative)
{
    unsigned int index;

    for (index = 0; index < count; ++index) {
        if (strcmp(queue[index].relative, relative) == 0)
            return 1;
    }
    return 0;
}

static int search_bounded(unsigned int source,
                          source_media_content_t content,
                          char *base, size_t base_size)
{
    search_directory_t queue[SEARCH_MAX_DIRECTORIES];
    unsigned int head = 0;
    unsigned int tail = 1;
    unsigned int entries = 0;
    unsigned int matches = 0;
    char match[192] = "";

    memset(queue, 0, sizeof(queue));
    while (head < tail && entries < SEARCH_MAX_ENTRIES) {
        char current[PATH_BUFFER_SIZE];
        int fd;
        iox_dirent_t entry;

        build_path(current, sizeof(current), sources[source].device,
                   queue[head].relative, "");
        fd = fileXioDopen(current);
        if (fd >= 0) {
            int read_result;

            for (;;) {
                char child[192];

                memset(&entry, 0, sizeof(entry));
                read_result = fileXioDread(fd, &entry);
                if (source_media_dread_is_eof(read_result))
                    break;
                if (read_result < 0 || ++entries > SEARCH_MAX_ENTRIES)
                    break;
                if (entry.name[0] == '\0' || strcmp(entry.name, ".") == 0 ||
                    strcmp(entry.name, "..") == 0 || strchr(entry.name, '/') != NULL ||
                    strchr(entry.name, '\\') != NULL || strchr(entry.name, ':') != NULL)
                    continue;
                if (!build_relative(child, sizeof(child),
                                    queue[head].relative, entry.name))
                    continue;
                if (base_has_content(source, content, child)) {
                    ++matches;
                    copy_text(match, sizeof(match), child);
                }
                if (queue[head].depth < SEARCH_MAX_DEPTH &&
                    tail < SEARCH_MAX_DIRECTORIES &&
                    !queue_contains(queue, tail, child)) {
                    char probe[PATH_BUFFER_SIZE];

                    build_path(probe, sizeof(probe), sources[source].device,
                               child, "");
                    if (directory_readable(probe)) {
                        copy_text(queue[tail].relative,
                                  sizeof(queue[tail].relative), child);
                        queue[tail].depth = queue[head].depth + 1u;
                        ++tail;
                    }
                }
            }
            (void)fileXioDclose(fd);
        }
        ++head;
    }
    if (matches == 1) {
        copy_text(base, base_size, match);
        return 1;
    }
    return matches > 1 ? -EEXIST : 0;
}

static int find_package_base(unsigned int source,
                             source_media_content_t content,
                             char *base, size_t base_size)
{
    int result = try_launch_ancestors(source, content, base, base_size);

    if (result != 0)
        return result;
    return search_bounded(source, content, base, base_size);
}

static int source_has_launch_file(unsigned int source)
{
    char path[PATH_BUFFER_SIZE];
    int fd;

    if (launch_file[0] == '\0')
        return 0;
    build_path(path, sizeof(path), sources[source].device,
               launch_directory, launch_file);
    fd = fileXioOpen(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    return fileXioClose(fd) >= 0;
}

static int execute_optional_module(unsigned char *data, unsigned int size)
{
    int startup = INT_MIN;
    int module_id = SifExecModuleBuffer(data, size, 0, NULL, &startup);

    return module_id < 0 ? module_id : startup;
}

int source_media_reuse_io_module(const char *name)
{
#if RBX_BUILD_MMCE
    if (!source_media_is_mmce() || iop_module_find("mmceman") < 0 ||
        iop_module_find("IOX/File_Manager_Rpc") < 0)
        return -1;
    if (strcmp(name, "iomanX") == 0)
        return iop_module_find("IO/File_Manager");
    if (strcmp(name, "fileXio") == 0)
        return iop_module_find("IOX/File_Manager_Rpc");
#else
    (void)name;
#endif
    return -1;
}

static int load_dependency(unsigned char *data, unsigned int size,
                           const char *module_name)
{
    int result = execute_optional_module(data, size);

    if (result != 0)
        return result < 0 ? result : -ENOEXEC;
    if (module_name == NULL)
        return 0;
    result = iop_module_find(module_name);
    return result >= 0 ? 0 : result;
}

int source_media_prepare_controller_stack(volatile int *stage)
{
    if (rejected_source >= 0)
        return -ENODEV;
#if RBX_BUILD_MMCE
    return source_media_prepare_mmce(stage);
#else
    int sio2, pad, result;

    if (active_source == 0)
        return 0;
    *stage = 20;
    sio2 = iop_module_find("sio2man");
    pad = iop_module_find("padman");
    if (sio2 >= 0 && pad >= 0)
        return 0;
    if (sio2 >= 0 || pad >= 0) {
        *stage = 29;
        return -EBUSY;
    }
    if (sio2 != -ENOENT || pad != -ENOENT)
        return -EIO;
    result = sbv_patch_enable_lmb();
    if (result < 0)
        return result;
    result = sbv_patch_disable_prefix_check();
    if (result < 0)
        return result;
    *stage = 21;
    result = load_dependency(sio2man_irx, size_sio2man_irx, "sio2man");
    if (result < 0)
        return result;
    DelayThread(1000000);
    *stage = 22;
    return load_dependency(padman_irx, size_padman_irx, "padman");
#endif
}

int source_media_prepare_mmce(volatile int *stage)
{
#if RBX_BUILD_MMCE
    int mmce, sio2, pad, filexio, result;

    *stage = 10;
    mmce = iop_module_find("mmceman");
    sio2 = iop_module_find("sio2man");
    pad = iop_module_find("padman");
    filexio = iop_module_find("IOX/File_Manager_Rpc");
    if (mmce >= 0)
        return sio2 >= 0 && pad >= 0 && filexio >= 0 ? 0 : -ENODEV;
    if (mmce != -ENOENT)
        return mmce;
    if (sio2 >= 0 || pad >= 0) {
        *stage = 19;
        return -EBUSY;
    }
    if (sio2 != -ENOENT || pad != -ENOENT ||
        (filexio < 0 && filexio != -ENOENT))
        return -EIO;
    result = sbv_patch_enable_lmb();
    if (result < 0)
        return result;
    result = sbv_patch_disable_prefix_check();
    if (result < 0)
        return result;
    if (filexio < 0) {
        *stage = 11;
        result = load_dependency(iomanX_irx, size_iomanX_irx,
                                 "IO/File_Manager");
        if (result < 0)
            return result;
        *stage = 12;
        result = load_dependency(fileXio_irx, size_fileXio_irx,
                                 "IOX/File_Manager_Rpc");
        if (result < 0)
            return result;
    }
    *stage = 13;
    result = load_dependency(sio2man_irx, size_sio2man_irx, "sio2man");
    if (result < 0)
        return result;
    DelayThread(1000000);
    *stage = 14;
    result = load_dependency(mmceman_irx, size_mmceman_irx, "mmceman");
    if (result < 0)
        return result;
    DelayThread(1000000);
    *stage = 15;
    return load_dependency(padman_irx, size_padman_irx, "padman");
#else
    (void)stage;
    return 0;
#endif
}

int source_media_load_optional_modules(void)
{
    int result = 0;

    if (optional_modules_loaded)
        return optional_modules_result;
    optional_modules_loaded = 1;
#if RBX_BUILD_MMCE
    result = iop_module_find("mmceman") >= 0 &&
             iop_module_find("padman") >= 0 ? 0 : -ENODEV;
#else
    result = load_dependency(bdm_irx, size_bdm_irx, NULL);
    if (result == 0)
        result = load_dependency(bdmfs_fatfs_irx,
                                 size_bdmfs_fatfs_irx, "bdmff");
    if (result == 0 && (active_source == 0 || unresolved_source_active))
        result = load_dependency(usbd_irx, size_usbd_irx, NULL);
    if (result == 0 && (active_source == 0 || unresolved_source_active))
        result = load_dependency(usbmass_bd_irx, size_usbmass_bd_irx, NULL);
    if (result == 0 && (active_source == 1 || unresolved_source_active))
        result = load_dependency(mx4sio_bd_irx, size_mx4sio_bd_irx, NULL);
    DelayThread(500000);
#endif
    optional_modules_result = result;
    return result;
}

int source_media_needs_optional_modules(void)
{
    return active_source >= 0;
}

int source_media_driver_result(void)
{
    return optional_modules_result;
}

int source_media_resolution_result(void)
{
    return resolution_result;
}

const char *source_media_resolution_name(void)
{
    if (resolution_result >= 0)
        return "READY";
    if (resolution_result == -ENOENT)
        return "PACKAGE NOT FOUND";
    if (resolution_result == -EEXIST)
        return "MULTIPLE PACKAGES";
    if (resolution_result == -ENODEV)
        return "WRONG ELF OR DEVICE";
    if (resolution_result == -EBUSY)
        return "DRIVER CONFLICT";
    return "SOURCE I/O ERROR";
}

void source_media_detect_preferred(int argc, char **argv)
{
    int index;

    preferred_source = -1;
    active_source = -1;
    rejected_source = -1;
    unresolved_source_active = 0;
    optional_modules_loaded = 0;
    optional_modules_result = 0;
    resolution_result = -ENODEV;
    media_ready = 0;
    launch_file[0] = '\0';
    launch_directory[0] = '\0';
    package_base[0] = '\0';
    if (argc <= 0 || argv == NULL)
        return;
    remember_launch_path(argv[0]);
    for (index = 0; index < argc && preferred_source < 0; ++index) {
        int identified = identify_source(argv[index]);

        if (identified >= 0) {
            if (source_index_allowed((unsigned int)identified))
                preferred_source = identified;
            else
                rejected_source = identified;
        }
    }
    if (preferred_source >= 0)
        set_active_source((unsigned int)preferred_source);
}

int source_media_prepare_unrecognized(void)
{
    if (active_source >= 0)
        return 0;
    if (rejected_source >= 0)
        return -ENODEV;
    set_active_source(RBX_BUILD_MMCE ? 2u : 1u);
    unresolved_source_active = 1;
    return 1;
}

int source_media_resolve_unrecognized(source_media_content_t content)
{
    int launch_match = -1;
    int content_match = -1;
    unsigned int launch_count = 0;
    unsigned int content_count = 0;
    unsigned int source;
    char matched_base[192] = "";

    if (!unresolved_source_active)
        return active_source >= 0 ? 0 : -ENODEV;
    if (optional_modules_result < 0)
        return optional_modules_result;
    for (source = RBX_FIRST_SOURCE_INDEX;
         source <= RBX_LAST_SOURCE_INDEX; ++source) {
        char found_base[192];
        int found;

        if (source_has_launch_file(source)) {
            launch_match = (int)source;
            ++launch_count;
        }
        found = find_package_base(source, content,
                                  found_base, sizeof(found_base));
        if (found < 0)
            return found;
        if (found > 0) {
            content_match = (int)source;
            ++content_count;
            copy_text(matched_base, sizeof(matched_base), found_base);
        }
    }
    if (launch_count == 1 && content_count == 1 &&
        launch_match != content_match)
        return -EEXIST;
    if (content_count == 1) {
        set_active_source((unsigned int)content_match);
        apply_package_base((unsigned int)content_match, matched_base);
        unresolved_source_active = 0;
        media_ready = 1;
        return 0;
    }
    if (content_count > 1 || launch_count > 1)
        return -EEXIST;
    return -ENOENT;
}

static void set_active_source(unsigned int index)
{
    active_source = (int)index;
    apply_package_base(index, "");
}

int source_media_select(source_media_content_t content)
{
    char found_base[192];
    int found;

    if (unresolved_source_active) {
        unsigned int attempt;

        for (attempt = 0; attempt < 12u; ++attempt) {
            resolution_result = source_media_resolve_unrecognized(content);
            if (resolution_result != -ENOENT)
                break;
            DelayThread(250000);
        }
        return resolution_result;
    }
    if (optional_modules_loaded && optional_modules_result < 0) {
        resolution_result = optional_modules_result;
        return resolution_result;
    }
    if (active_source < 0) {
        resolution_result = -ENODEV;
        return resolution_result;
    }
    if (!media_ready) {
        unsigned int attempt;
        char root[32];

        snprintf(root, sizeof(root), "%s/", sources[active_source].device);
        for (attempt = 0; attempt < 20u; ++attempt) {
            int fd = fileXioDopen(root);

            resolution_result = fd < 0 ? fd : fileXioDclose(fd);
            if (resolution_result >= 0) {
                resolution_result = 0;
                media_ready = 1;
                break;
            }
            DelayThread(250000);
        }
        if (!media_ready)
            return resolution_result;
    }
    {
        unsigned int attempt;

        found = 0;
        for (attempt = 0; attempt < 12u; ++attempt) {
            found = find_package_base((unsigned int)active_source, content,
                                      found_base, sizeof(found_base));
            if (found != 0)
                break;
            DelayThread(250000);
        }
    }
    if (found > 0) {
        apply_package_base((unsigned int)active_source, found_base);
        resolution_result = 0;
    } else {
        resolution_result = found < 0 ? found : -ENOENT;
    }
    return resolution_result;
}

const char *source_media_label(void)
{
    if (unresolved_source_active && active_source >= 0)
        return "DETECTING";
    return active_source >= 0 ? sources[active_source].label : "NOT FOUND";
}

const char *source_media_device_root(void) { return active_source >= 0 ? sources[active_source].device : ""; }
const char *source_media_psx1_system_root(void) { return psx1_root; }
const char *source_media_psx2_system_root(void) { return psx2_root; }
const char *source_media_psx1_xfrom_root(void) { return psx1_xfrom_root; }
const char *source_media_psx2_xfrom_root(void) { return psx2_xfrom_root; }
const char *source_media_bootstrap_root(void) { return bootstrap_root; }
const char *source_media_bootstrap_bin_path(void) { return bootstrap_bin; }
const char *source_media_bootstrap_sums_path(void) { return bootstrap_sums; }
const char *source_media_apps_root(void) { return apps_root; }

int source_media_is_mmce(void) { return active_source >= 2; }
int source_media_is_selected(void) { return active_source >= 0; }

int source_media_dread_is_eof(int result)
{
    return result == 0 || (source_media_is_mmce() && result == -1);
}

size_t source_media_read_size(size_t requested)
{
    return source_media_is_mmce() && requested > MMCE_READ_SIZE
               ? MMCE_READ_SIZE : requested;
}
