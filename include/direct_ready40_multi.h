#ifndef REPAIRBOX_DIRECT_READY40_MULTI_H
#define REPAIRBOX_DIRECT_READY40_MULTI_H

#include "direct_ready40.h"

void dr40_multi_scan_preflight(dr40_result_t *result,
                               storage_diagnostics_t *diagnostics);
void dr40_multi_execute(dr40_result_t *result,
                        storage_diagnostics_t *diagnostics,
                        dr40_progress_callback_t progress, void *context);
int dr40_multi_exact_dvr_layout(const storage_device_result_t *device,
                                u32 native_max, u32 *end, u32 *tail);

#endif
