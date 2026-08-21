#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <hdd-ioctl.h>
#include <io_common.h>

#include "bootstrap.h"
#include "sha256.h"

#define BOOTSTRAP_IO_SECTORS 3u
#define BOOTSTRAP_IO_BYTES (BOOTSTRAP_IO_SECTORS * 512u)

static unsigned char file_buffer[64u * 1024u] __attribute__((aligned(64)));
static unsigned char readback_buffer[BOOTSTRAP_IO_BYTES]
    __attribute__((aligned(64)));
static unsigned char transfer_buffer[sizeof(hddAtaTransfer_t) +
                                     BOOTSTRAP_IO_BYTES]
    __attribute__((aligned(64)));

static u32 elapsed_ms(u64 start, u64 end)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(end - start, &seconds, &microseconds);
    return seconds * 1000u + microseconds / 1000u;
}

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int parse_sum(const char *line,
                     unsigned char digest[BOOTSTRAP_SHA256_SIZE])
{
    unsigned int index;

    for (index = 0; index < BOOTSTRAP_SHA256_SIZE; ++index) {
        int high = hex_value((unsigned char)line[index * 2u]);
        int low = hex_value((unsigned char)line[index * 2u + 1u]);

        if (high < 0 || low < 0)
            return -EINVAL;
        digest[index] = (unsigned char)((high << 4) | low);
    }
    if (line[64] != ' ' || line[65] != ' ' ||
        strcmp(line + 66, "mbr_bootstrap_prefix.bin\n") != 0)
        return -EINVAL;
    return 0;
}

void bootstrap_initialize(bootstrap_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->binary_open_result = -1;
    result->binary_final_read_result = -1;
    result->binary_close_result = -1;
    result->sums_open_result = -1;
    result->sums_read_result = -1;
    result->sums_close_result = -1;
    result->sums_parse_result = -1;
    result->write_return = -1;
    result->readback_return = -1;
}

void bootstrap_validate_usb(bootstrap_result_t *result)
{
    sha256_context_t hash;
    char sums_line[96];
    int fd;

    result->validation_attempted = 1;
    sha256_init(&hash);
    fd = fileXioOpen(BOOTSTRAP_BIN_PATH, FIO_O_RDONLY, 0);
    result->binary_open_result = fd;
    if (fd < 0)
        return;
    for (;;) {
        int read_result = fileXioRead(fd, file_buffer, sizeof(file_buffer));

        result->binary_final_read_result = read_result;
        if (read_result <= 0)
            break;
        result->binary_size += (u32)read_result;
        sha256_update(&hash, file_buffer, (size_t)read_result);
    }
    result->binary_close_result = fileXioClose(fd);
    sha256_final(&hash, result->source_sha256);
    result->size_valid = result->binary_final_read_result == 0 &&
                         result->binary_close_result >= 0 &&
                         result->binary_size == BOOTSTRAP_BYTE_COUNT;

    memset(sums_line, 0, sizeof(sums_line));
    fd = fileXioOpen(BOOTSTRAP_SUMS_PATH, FIO_O_RDONLY, 0);
    result->sums_open_result = fd;
    if (fd < 0)
        return;
    result->sums_read_result = fileXioRead(fd, sums_line,
                                          sizeof(sums_line) - 1u);
    result->sums_close_result = fileXioClose(fd);
    if (result->sums_read_result > 0 &&
        result->sums_read_result < (int)sizeof(sums_line)) {
        sums_line[result->sums_read_result] = '\0';
        result->sums_parse_result =
            parse_sum(sums_line, result->expected_sha256);
    }
    result->sha256_valid = result->sums_parse_result == 0 &&
        memcmp(result->expected_sha256, result->source_sha256,
               BOOTSTRAP_SHA256_SIZE) == 0;
    result->validation_valid = result->size_valid && result->sha256_valid;
}

int bootstrap_write_and_verify(bootstrap_result_t *result)
{
    static const u32 samples[] = {0u, 0x1002u, 0x2000u,
                                  0x2020u, 0x2440u, 0x27E6u};
    unsigned char sample_source[sizeof(samples) / sizeof(samples[0])][512];
    sha256_context_t readback_hash;
    u64 start;
    u64 end;
    u32 lba = 0;
    int fd;
    unsigned int sample_index;

    if (!result->validation_valid)
        return -EPERM;
    result->write_attempted = 1;
    fd = fileXioOpen(BOOTSTRAP_BIN_PATH, FIO_O_RDONLY, 0);
    if (fd < 0) {
        result->write_return = fd;
        return fd;
    }
    start = GetTimerSystemTime();
    while (lba < BOOTSTRAP_SECTOR_COUNT) {
        hddAtaTransfer_t *transfer = (hddAtaTransfer_t *)transfer_buffer;
        u32 sectors = BOOTSTRAP_SECTOR_COUNT - lba;
        u32 bytes;
        int read_result;
        int write_result;

        if (sectors > BOOTSTRAP_IO_SECTORS)
            sectors = BOOTSTRAP_IO_SECTORS;
        bytes = sectors * 512u;
        read_result = fileXioRead(fd, transfer->data, bytes);
        if (read_result != (int)bytes) {
            result->failure_lba = lba;
            result->failure_requested_bytes = bytes;
            result->failure_actual_bytes = read_result;
            result->write_return = read_result < 0 ? read_result : -EIO;
            fileXioClose(fd);
            return result->write_return;
        }
        for (sample_index = 0;
             sample_index < sizeof(samples) / sizeof(samples[0]);
             ++sample_index) {
            if (samples[sample_index] >= lba &&
                samples[sample_index] < lba + sectors) {
                memcpy(sample_source[sample_index],
                       transfer->data + (samples[sample_index] - lba) * 512u,
                       512u);
            }
        }
        transfer->lba = lba;
        transfer->size = sectors;
        write_result = fileXioDevctl(
            "hdd0:", HDIOC_WRITESECTOR, transfer,
            sizeof(*transfer) + bytes, NULL, 0);
        if (write_result < 0) {
            result->failure_lba = lba;
            result->failure_requested_bytes = bytes;
            result->failure_actual_bytes = write_result;
            result->write_return = write_result;
            fileXioClose(fd);
            return write_result;
        }
        lba += sectors;
        result->written_sectors = lba;
    }
    result->write_return = fileXioClose(fd);
    end = GetTimerSystemTime();
    result->write_duration_ms = elapsed_ms(start, end);
    if (result->write_return < 0)
        return result->write_return;

    sha256_init(&readback_hash);
    lba = 0;
    result->sample_sectors_valid = 1;
    while (lba < BOOTSTRAP_SECTOR_COUNT) {
        hddAtaTransfer_t request;
        u32 sectors = BOOTSTRAP_SECTOR_COUNT - lba;
        u32 bytes;
        int read_result;

        if (sectors > BOOTSTRAP_IO_SECTORS)
            sectors = BOOTSTRAP_IO_SECTORS;
        bytes = sectors * 512u;
        request.lba = lba;
        request.size = sectors;
        read_result = fileXioDevctl(
            "hdd0:", HDIOC_READSECTOR, &request, sizeof(request),
            readback_buffer, bytes);
        if (read_result < 0) {
            result->failure_lba = lba;
            result->failure_requested_bytes = bytes;
            result->failure_actual_bytes = read_result;
            result->readback_return = read_result;
            return read_result;
        }
        sha256_update(&readback_hash, readback_buffer, bytes);
        for (sample_index = 0;
             sample_index < sizeof(samples) / sizeof(samples[0]);
             ++sample_index) {
            if (samples[sample_index] >= lba &&
                samples[sample_index] < lba + sectors &&
                memcmp(sample_source[sample_index],
                       readback_buffer +
                           (samples[sample_index] - lba) * 512u,
                       512u) != 0)
                result->sample_sectors_valid = 0;
        }
        lba += sectors;
        result->readback_sectors = lba;
    }
    sha256_final(&readback_hash, file_buffer);
    result->readback_sha256_valid =
        memcmp(file_buffer, result->expected_sha256,
               BOOTSTRAP_SHA256_SIZE) == 0;
    result->readback_return = 0;
    result->bootstrap_valid = result->readback_sha256_valid &&
                              result->sample_sectors_valid;
    return result->bootstrap_valid ? 0 : -EIO;
}
