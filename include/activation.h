#ifndef REPAIRBOX_PSX2_ACTIVATION_H
#define REPAIRBOX_PSX2_ACTIVATION_H

#include <tamtypes.h>
#include "bootflag.h"

typedef struct activation_file {
    u8 bytes[BOOTFLAG_SIZE];
    u32 size;
    int open_result;
    int read_result;
    int extra_read_result;
    int close_result;
    u8 sha1[BOOTFLAG_DIGEST_SIZE];
    u8 sha256[32];
    bootflag_inspection_t inspection;
} activation_file_t;

typedef struct activation_result {
    activation_file_t original;
    activation_file_t pending_readback;
    bootflag_image_t generated;
    bootflag_inspection_t generated_inspection;
    int generation_valid;
    int write_attempted;
    int write_open_result;
    int write_result;
    u32 bytes_written;
    int write_close_result;
    int pending_valid;
    int sony_activation_armed;
    int current_valid;
    int already_armed;
    u32 preparation_duration_ms;
    u32 activation_duration_ms;
} activation_result_t;

void activation_initialize(activation_result_t *result);
void activation_read_current(activation_result_t *result);
void activation_prepare(activation_result_t *result);
int activation_arm_pending(activation_result_t *result,
                           int storage_valid, int session_confirmed);
int activation_verify_existing_pending(activation_result_t *result,
                                       int storage_valid,
                                       int session_confirmed);

#endif
