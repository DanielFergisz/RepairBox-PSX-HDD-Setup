#include <errno.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "activation.h"
#include "sha256.h"

static u32 elapsed_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static void hash_file(activation_file_t *file)
{
    sha256_context_t hash;

    bootflag_sha1(file->bytes + BOOTFLAG_PAYLOAD_OFFSET,
                  BOOTFLAG_PAYLOAD_SIZE, file->sha1);
    sha256_init(&hash);
    sha256_update(&hash, file->bytes, BOOTFLAG_SIZE);
    sha256_final(&hash, file->sha256);
}

static void read_image(const char *path, activation_file_t *file)
{
    u8 extra = 0;
    int fd;
    int offset = 0;

    memset(file, 0, sizeof(*file));
    file->open_result = -1;
    file->read_result = -1;
    file->extra_read_result = -1;
    file->close_result = -1;
    fd = fileXioOpen(path, FIO_O_RDONLY, 0);
    file->open_result = fd;
    if (fd < 0)
        return;
    while (offset < BOOTFLAG_SIZE) {
        int amount = fileXioRead(fd, file->bytes + offset,
                                 BOOTFLAG_SIZE - offset);

        file->read_result = amount;
        if (amount <= 0)
            break;
        offset += amount;
    }
    if (offset == BOOTFLAG_SIZE)
        file->extra_read_result = fileXioRead(fd, &extra, 1);
    file->close_result = fileXioClose(fd);
    file->size = (u32)offset +
                 (file->extra_read_result > 0
                      ? (u32)file->extra_read_result : 0u);
    bootflag_inspect_image(
        file->bytes,
        offset == BOOTFLAG_SIZE && file->extra_read_result == 0
            ? BOOTFLAG_SIZE : file->size,
        &file->inspection);
    if (offset == BOOTFLAG_SIZE && file->extra_read_result == 0 &&
        file->close_result >= 0)
        hash_file(file);
}

static int image_valid(const activation_file_t *file)
{
    return file->open_result >= 0 && file->size == BOOTFLAG_SIZE &&
           file->extra_read_result == 0 && file->close_result >= 0 &&
           file->inspection.sha1_valid && file->inspection.payload_valid &&
           !file->inspection.conflicting_value &&
           file->inspection.state != BOOTFLAG_STATE_INVALID;
}

static int write_all(int fd, const u8 *bytes, u32 size, u32 *written)
{
    u32 offset = 0;

    *written = 0;
    while (offset < size) {
        int result = fileXioWrite(fd, bytes + offset, (int)(size - offset));

        if (result <= 0)
            return result < 0 ? result : -EIO;
        offset += (u32)result;
        *written += (u32)result;
    }
    return (int)offset;
}

void activation_initialize(activation_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->write_open_result = -1;
    result->write_result = -1;
    result->write_close_result = -1;
}

void activation_read_current(activation_result_t *result)
{
    read_image(BOOTFLAG_XFROM_PATH, &result->original);
}

void activation_prepare(activation_result_t *result)
{
    u64 start = GetTimerSystemTime();

    result->current_valid = image_valid(&result->original);
    if (!result->current_valid)
        return;
    if (result->original.inspection.state == BOOTFLAG_STATE_PENDING_40GB) {
        result->already_armed =
            memcmp(result->original.sha1, bootflag_expected_sha1,
                   BOOTFLAG_DIGEST_SIZE) == 0 &&
            result->original.inspection.unknown_key_count == 0;
        result->preparation_duration_ms =
            elapsed_ms(start, GetTimerSystemTime());
        return;
    }
    if (result->original.inspection.state != BOOTFLAG_STATE_NORMAL)
        return;
    if (bootflag_generate_standard_40gb(&result->generated) == 0) {
        bootflag_inspect_image(result->generated.bytes, BOOTFLAG_SIZE,
                               &result->generated_inspection);
        result->generation_valid =
            memcmp(result->generated.digest, bootflag_expected_sha1,
                   BOOTFLAG_DIGEST_SIZE) == 0 &&
            result->generated_inspection.state ==
                BOOTFLAG_STATE_PENDING_40GB &&
            result->generated_inspection.bootmode_normal &&
            result->generated_inspection.repartition_40 &&
            result->generated_inspection.contents_1 &&
            result->generated_inspection.unknown_key_count == 0;
    }
    result->preparation_duration_ms =
        elapsed_ms(start, GetTimerSystemTime());
}

int activation_arm_pending(activation_result_t *result,
                           int storage_valid, int session_confirmed)
{
    int fd;
    u64 start;

    if (!storage_valid || !session_confirmed ||
        !result->generation_valid ||
        result->original.inspection.state != BOOTFLAG_STATE_NORMAL)
        return -EPERM;
    result->write_attempted = 1;
    start = GetTimerSystemTime();
    fd = fileXioOpen(BOOTFLAG_XFROM_PATH,
                     FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    result->write_open_result = fd;
    if (fd < 0) {
        result->activation_duration_ms =
            elapsed_ms(start, GetTimerSystemTime());
        return fd;
    }
    result->write_result =
        write_all(fd, result->generated.bytes, BOOTFLAG_SIZE,
                  &result->bytes_written);
    result->write_close_result = fileXioClose(fd);
    if (result->write_result != BOOTFLAG_SIZE ||
        result->bytes_written != BOOTFLAG_SIZE ||
        result->write_close_result < 0) {
        result->activation_duration_ms =
            elapsed_ms(start, GetTimerSystemTime());
        return result->write_result < 0 ? result->write_result : -EIO;
    }
    read_image(BOOTFLAG_XFROM_PATH, &result->pending_readback);
    result->pending_valid =
        image_valid(&result->pending_readback) &&
        memcmp(result->pending_readback.bytes, result->generated.bytes,
               BOOTFLAG_SIZE) == 0 &&
        memcmp(result->pending_readback.sha1, bootflag_expected_sha1,
               BOOTFLAG_DIGEST_SIZE) == 0 &&
        result->pending_readback.inspection.state ==
            BOOTFLAG_STATE_PENDING_40GB &&
        result->pending_readback.inspection.unknown_key_count == 0;
    result->sony_activation_armed = result->pending_valid;
    result->activation_duration_ms = elapsed_ms(start, GetTimerSystemTime());
    return result->pending_valid ? 0 : -EIO;
}

int activation_verify_existing_pending(activation_result_t *result,
                                       int storage_valid,
                                       int session_confirmed)
{
    u64 start;

    if (!storage_valid || !session_confirmed || !result->already_armed)
        return -EPERM;
    start = GetTimerSystemTime();
    read_image(BOOTFLAG_XFROM_PATH, &result->pending_readback);
    result->pending_valid =
        image_valid(&result->pending_readback) &&
        memcmp(result->pending_readback.bytes, result->original.bytes,
               BOOTFLAG_SIZE) == 0 &&
        memcmp(result->pending_readback.sha1, bootflag_expected_sha1,
               BOOTFLAG_DIGEST_SIZE) == 0 &&
        result->pending_readback.inspection.state ==
            BOOTFLAG_STATE_PENDING_40GB &&
        result->pending_readback.inspection.unknown_key_count == 0;
    result->sony_activation_armed = result->pending_valid;
    result->activation_duration_ms = elapsed_ms(start, GetTimerSystemTime());
    return result->pending_valid ? 0 : -EIO;
}
