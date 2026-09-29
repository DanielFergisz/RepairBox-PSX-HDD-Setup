#ifndef REPAIRBOX_SOURCE_MEDIA_H
#define REPAIRBOX_SOURCE_MEDIA_H

#include <stddef.h>

typedef enum source_media_content {
    SOURCE_MEDIA_PSX1_SYSTEM = 0,
    SOURCE_MEDIA_PSX2_SYSTEM,
    SOURCE_MEDIA_APPS
} source_media_content_t;

void source_media_detect_preferred(int argc, char **argv);
int source_media_prepare_unrecognized(void);
int source_media_resolve_unrecognized(source_media_content_t content);
int source_media_prepare_controller_stack(volatile int *stage);
int source_media_prepare_mmce(volatile int *stage);
int source_media_load_optional_modules(void);
int source_media_needs_optional_modules(void);
int source_media_driver_result(void);
int source_media_resolution_result(void);
const char *source_media_resolution_name(void);
int source_media_reuse_io_module(const char *name);
int source_media_select(source_media_content_t content);
const char *source_media_label(void);
const char *source_media_device_root(void);
const char *source_media_psx1_system_root(void);
const char *source_media_psx2_system_root(void);
const char *source_media_psx1_xfrom_root(void);
const char *source_media_psx2_xfrom_root(void);
const char *source_media_bootstrap_root(void);
const char *source_media_bootstrap_bin_path(void);
const char *source_media_bootstrap_sums_path(void);
const char *source_media_apps_root(void);
int source_media_is_mmce(void);
int source_media_is_selected(void);
int source_media_dread_is_eof(int result);
size_t source_media_read_size(size_t requested);

#endif
