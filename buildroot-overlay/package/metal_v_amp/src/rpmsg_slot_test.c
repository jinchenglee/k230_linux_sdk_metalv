#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rpmsg_protocol.h"
#include "rpmsg_slot_state.h"
#ifdef K230_SLOT_CAMERA
#include "mmz.h"
#include "v4l2-drm.h"
#endif

#define RPMSG_MAX_PAYLOAD 496U
#define BIG_CORE_HZ 1600000000.0

struct expected_reply {
    uint64_t sequence;
    uint32_t crc;
    uint32_t slot_id;
    uint32_t seen;
};

static double elapsed_ms(const struct timespec *start,
                         const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

#ifdef K230_SLOT_CAMERA
static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}
#endif

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    unsigned bit;

    crc ^= byte;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^
              (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    return crc;
}

static uint32_t fill_pattern(uint8_t *slot, uint32_t data_length,
                             uint32_t padded_length, uint32_t seed)
{
    uint32_t crc = UINT32_C(0xffffffff);
    uint32_t state = seed ? seed : 1U;
    uint32_t i;

    for (i = 0; i < data_length; ++i) {
        uint8_t byte;

        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        byte = (uint8_t)(state >> 24);
        slot[i] = byte;
        crc = crc32_byte(crc, byte);
    }
    for (; i < padded_length; ++i)
        slot[i] = 0;
    __sync_synchronize();
    return ~crc;
}

static uint32_t padded_length(uint32_t length)
{
    return (length + K230_PAYLOAD_CACHE_LINE - 1U) &
           ~(K230_PAYLOAD_CACHE_LINE - 1U);
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);

    while (length--)
        crc = crc32_byte(crc, *data++);
    return ~crc;
}

static void request_header(struct k230_rpmsg_protocol_header *header,
                           uint16_t type, uint64_t generation,
                           uint64_t sequence, uint64_t capabilities,
                           uint32_t message_size)
{
    memset(header, 0, sizeof(*header));
    header->magic = K230_RPMSG_PROTOCOL_MAGIC;
    header->version = K230_RPMSG_PROTOCOL_VERSION;
    header->header_size = K230_RPMSG_PROTOCOL_HEADER_SIZE;
    header->type = type;
    header->message_size = message_size;
    header->generation = generation;
    header->sequence = sequence;
    header->capabilities = capabilities;
}

static int send_request(int fd, const void *request, size_t length)
{
    ssize_t written = write(fd, request, length);

    if (written == (ssize_t)length)
        return 0;
    fprintf(stderr, "RPMsg write failed: %s\n",
            written < 0 ? strerror(errno) : "short write");
    return 1;
}

static int receive_reply(int fd, unsigned char *reply, size_t *length,
                         int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready;
    ssize_t received;

    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        fprintf(stderr, "RPMsg reply %s\n",
                ready == 0 ? "timed out" : strerror(errno));
        return 1;
    }
    received = read(fd, reply, RPMSG_MAX_PAYLOAD);
    if (received < 0) {
        fprintf(stderr, "RPMsg read failed: %s\n", strerror(errno));
        return 1;
    }
    *length = (size_t)received;
    return 0;
}

static int check_header(const unsigned char *data, size_t length,
                        struct k230_rpmsg_protocol_header *header)
{
    if (length < sizeof(*header)) {
        fprintf(stderr, "short protocol reply: %zu\n", length);
        return 1;
    }
    memcpy(header, data, sizeof(*header));
    if (header->magic != K230_RPMSG_PROTOCOL_MAGIC ||
        header->version != K230_RPMSG_PROTOCOL_VERSION ||
        header->header_size != sizeof(*header) ||
        header->message_size != length) {
        fprintf(stderr,
                "bad reply header magic=%08x version=%u header=%u "
                "size=%u/%zu\n",
                header->magic, header->version, header->header_size,
                header->message_size, length);
        return 1;
    }
    return 0;
}

static int exchange_header(int fd,
                           const struct k230_rpmsg_protocol_header *request,
                           uint16_t expected_type, uint16_t expected_status,
                           uint64_t *reply_generation, int timeout_ms)
{
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct k230_rpmsg_protocol_header reply;
    size_t length;

    if (send_request(fd, request, request->message_size) ||
        receive_reply(fd, data, &length, timeout_ms) ||
        check_header(data, length, &reply))
        return 1;
    if (reply.sequence != request->sequence ||
        reply.type != expected_type || reply.status != expected_status) {
        fprintf(stderr,
                "unexpected reply seq=%" PRIu64 "/%" PRIu64
                " type=%u/%u status=%u/%u\n",
                reply.sequence, request->sequence, reply.type,
                expected_type, reply.status, expected_status);
        return 1;
    }
    if (reply_generation)
        *reply_generation = reply.generation;
    return 0;
}

static int handshake(int fd, uint64_t sequence, uint64_t *generation,
                     int timeout_ms)
{
    struct k230_rpmsg_protocol_header request;
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct k230_rpmsg_protocol_header reply;
    uint64_t required = K230_RPMSG_REQUIRED_CAPABILITIES |
                        K230_RPMSG_CAP_PAYLOAD_SLOTS;
    size_t length;

    request_header(&request, K230_RPMSG_MSG_HELLO, 0, sequence, required,
                   sizeof(request));
    if (send_request(fd, &request, sizeof(request)) ||
        receive_reply(fd, data, &length, timeout_ms) ||
        check_header(data, length, &reply))
        return 1;
    if (reply.sequence != sequence ||
        reply.type != K230_RPMSG_MSG_HELLO_REPLY ||
        reply.status != K230_RPMSG_STATUS_OK || !reply.generation ||
        (reply.capabilities & required) != required) {
        fprintf(stderr, "payload-slot capability handshake failed\n");
        return 1;
    }
    *generation = reply.generation;
    return 0;
}

static void slot_request(struct k230_rpmsg_slot_submit *request,
                         uint64_t generation, uint64_t sequence,
                         uint32_t slot_id, uint32_t data_length,
                         uint32_t crc, uint32_t format,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    memset(request, 0, sizeof(*request));
    request_header(&request->header, K230_RPMSG_MSG_SLOT_SUBMIT,
                   generation, sequence, 0, sizeof(*request));
    request->slot_id = slot_id;
    request->offset = (uint64_t)slot_id * K230_PAYLOAD_SLOT_SIZE;
    request->data_length = data_length;
    request->padded_length = padded_length(data_length);
    request->expected_crc = crc;
    request->format = format;
    request->width = width;
    request->height = height;
    request->stride = stride;
}

static int run_self_test(void)
{
    static const uint8_t crc_fixture[] = "123456789";
    struct k230_rpmsg_slot_submit request;
    struct k230_rpmsg_camera_register camera_register = { 0 };
    struct k230_rpmsg_camera_submit camera_submit = { 0 };
    struct k230_slot_ownership ownership = { 0 };
    unsigned slot;
    int failed = 0;

    failed |= sizeof(request) != K230_RPMSG_SLOT_SUBMIT_SIZE;
    failed |= sizeof(struct k230_rpmsg_slot_complete) !=
              K230_RPMSG_SLOT_COMPLETE_SIZE;
    failed |= sizeof(camera_register) != K230_RPMSG_CAMERA_REGISTER_SIZE;
    failed |= sizeof(camera_submit) != K230_RPMSG_CAMERA_SUBMIT_SIZE;
    failed |= sizeof(struct k230_rpmsg_camera_complete) !=
              K230_RPMSG_CAMERA_COMPLETE_SIZE;
    failed |= padded_length(1) != 64 || padded_length(64) != 64 ||
              padded_length(65) != 128;
    failed |= crc32(crc_fixture, sizeof(crc_fixture) - 1) !=
              UINT32_C(0xcbf43926);

    slot_request(&request, 1, 1, 3, 1280U * 720U, 0,
                 K230_PAYLOAD_FORMAT_Y8, 1280, 720, 1280);
    failed |= k230_rpmsg_validate_slot(&request) != K230_RPMSG_STATUS_OK;
    request.slot_id = K230_PAYLOAD_SLOT_COUNT;
    failed |= k230_rpmsg_validate_slot(&request) !=
              K230_RPMSG_STATUS_INVALID_SLOT;
    request.slot_id = 3;
    request.offset++;
    failed |= k230_rpmsg_validate_slot(&request) !=
              K230_RPMSG_STATUS_INVALID_RANGE;
    request.offset--;
    request.stride = 1279;
    failed |= k230_rpmsg_validate_slot(&request) !=
              K230_RPMSG_STATUS_BAD_FORMAT;

    camera_register.buffer_id = K230_CAMERA_BUFFER_COUNT - 1U;
    camera_register.physical = K230_CAMERA_POOL_BASE +
        (K230_CAMERA_BUFFER_COUNT - 1U) * K230_CAMERA_BUFFER_SIZE;
    camera_register.capacity = K230_CAMERA_BUFFER_SIZE;
    failed |= k230_rpmsg_validate_camera_registration(&camera_register) !=
              K230_RPMSG_STATUS_OK;
    ++camera_register.physical;
    failed |= k230_rpmsg_validate_camera_registration(&camera_register) !=
              K230_RPMSG_STATUS_INVALID_RANGE;

    camera_submit.buffer_id = K230_CAMERA_BUFFER_COUNT - 1U;
    camera_submit.data_length = 1280U * 720U;
    camera_submit.padded_length = padded_length(camera_submit.data_length);
    camera_submit.format = K230_PAYLOAD_FORMAT_Y8;
    camera_submit.width = 1280;
    camera_submit.height = 720;
    camera_submit.stride = 1280;
    failed |= k230_rpmsg_validate_camera_submit(&camera_submit) !=
              K230_RPMSG_STATUS_OK;
    camera_submit.padded_length++;
    failed |= k230_rpmsg_validate_camera_submit(&camera_submit) !=
              K230_RPMSG_STATUS_INVALID_RANGE;

    for (slot = 0; slot < K230_PAYLOAD_SLOT_COUNT; ++slot)
        failed |= k230_slot_claim(&ownership, slot) !=
                  K230_RPMSG_STATUS_OK;
    failed |= ownership.depth != K230_PAYLOAD_SLOT_COUNT ||
              ownership.high_water != K230_PAYLOAD_SLOT_COUNT ||
              ownership.busy_mask !=
                  ((UINT32_C(1) << K230_PAYLOAD_SLOT_COUNT) - 1U);
    failed |= k230_slot_claim(&ownership, 0) !=
              K230_RPMSG_STATUS_SLOT_BUSY;
    failed |= !k230_slot_release(&ownership, 2) ||
              ownership.depth != K230_PAYLOAD_SLOT_COUNT - 1U;
    failed |= k230_slot_claim(&ownership, 2) != K230_RPMSG_STATUS_OK;
    failed |= k230_slot_reset(&ownership) != K230_PAYLOAD_SLOT_COUNT ||
              ownership.depth || ownership.busy_mask ||
              ownership.high_water != K230_PAYLOAD_SLOT_COUNT;

    printf("%s payload-slot/camera host ABI/CRC/descriptor/ownership checks\n",
           failed ? "FAIL" : "PASS");
    return failed;
}

static int check_completion(const unsigned char *data, size_t length,
                            const struct k230_rpmsg_slot_submit *request,
                            uint16_t expected_status,
                            struct k230_rpmsg_slot_complete *complete)
{
    struct k230_rpmsg_protocol_header header;

    if (check_header(data, length, &header))
        return 1;
    if (length != sizeof(*complete) ||
        header.type != K230_RPMSG_MSG_SLOT_COMPLETE ||
        header.status != expected_status ||
        header.sequence != request->header.sequence ||
        header.generation != request->header.generation) {
        fprintf(stderr,
                "bad slot completion seq=%" PRIu64 "/%" PRIu64
                " type=%u status=%u size=%zu\n",
                header.sequence, request->header.sequence, header.type,
                header.status, length);
        return 1;
    }
    memcpy(complete, data, sizeof(*complete));
    if (complete->slot_id != request->slot_id ||
        complete->offset != request->offset ||
        complete->data_length != request->data_length ||
        complete->observed_crc != request->expected_crc) {
        fprintf(stderr,
                "slot completion mismatch slot=%u/%u offset=%" PRIu64
                "/%" PRIu64 " length=%u/%u crc=%08x/%08x\n",
                complete->slot_id, request->slot_id, complete->offset,
                request->offset, complete->data_length,
                request->data_length, complete->observed_crc,
                request->expected_crc);
        return 1;
    }
    return 0;
}

static int run_one(int fd, uint8_t *pool, uint64_t generation,
                   uint64_t sequence, uint32_t slot_id,
                   uint32_t data_length, uint32_t format,
                   uint32_t width, uint32_t height, uint32_t stride,
                   int timeout_ms)
{
    struct k230_rpmsg_slot_submit request;
    struct k230_rpmsg_slot_complete complete;
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct timespec start, end;
    uint8_t *slot = pool + (size_t)slot_id * K230_PAYLOAD_SLOT_SIZE;
    uint32_t crc;
    size_t length;
    double ms;

    crc = fill_pattern(slot, data_length, padded_length(data_length),
                       (uint32_t)sequence ^ (slot_id << 24));
    slot_request(&request, generation, sequence, slot_id, data_length, crc,
                 format, width, height, stride);
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (send_request(fd, &request, sizeof(request)) ||
        receive_reply(fd, data, &length, timeout_ms))
        return 1;
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (check_completion(data, length, &request, K230_RPMSG_STATUS_OK,
                         &complete))
        return 1;
    ms = elapsed_ms(&start, &end);
    printf("PASS slot=%u bytes=%u descriptor-rtt=%.3f ms "
           "effective=%.2f MiB/s big-cycles{invalidate=%" PRIu64
           ",crc=%" PRIu64 "} big-ms{invalidate=%.3f,crc=%.3f}\n",
           slot_id, data_length, ms,
           data_length / (ms * 1048.576), complete.invalidate_cycles,
           complete.crc_cycles,
           complete.invalidate_cycles * 1000.0 / BIG_CORE_HZ,
           complete.crc_cycles * 1000.0 / BIG_CORE_HZ);
    return 0;
}

static int expect_error(int fd, struct k230_rpmsg_slot_submit *request,
                        uint16_t status, int timeout_ms)
{
    return exchange_header(fd, &request->header, K230_RPMSG_MSG_ERROR,
                           status, NULL, timeout_ms);
}

static int run_negative_tests(int fd, uint64_t generation, int timeout_ms)
{
    struct k230_rpmsg_slot_submit request;
    int failed = 0;

    slot_request(&request, generation, 100, K230_PAYLOAD_SLOT_COUNT, 64, 0,
                 K230_PAYLOAD_FORMAT_OPAQUE, 0, 0, 0);
    failed |= expect_error(fd, &request, K230_RPMSG_STATUS_INVALID_SLOT,
                           timeout_ms);

    slot_request(&request, generation, 101, 0, 64, 0,
                 K230_PAYLOAD_FORMAT_OPAQUE, 0, 0, 0);
    request.offset = 1;
    failed |= expect_error(fd, &request, K230_RPMSG_STATUS_INVALID_RANGE,
                           timeout_ms);

    slot_request(&request, generation, 102, 0, 64, 0,
                 K230_PAYLOAD_FORMAT_Y8, 8, 8, 7);
    failed |= expect_error(fd, &request, K230_RPMSG_STATUS_BAD_FORMAT,
                           timeout_ms);
    printf("%s invalid slot/range/format rejection\n",
           failed ? "FAIL" : "PASS");
    return failed;
}

static int find_expected(struct expected_reply *expected, unsigned count,
                         uint64_t sequence)
{
    unsigned i;

    for (i = 0; i < count; ++i)
        if (expected[i].sequence == sequence)
            return (int)i;
    return -1;
}

static int run_queue_test(int fd, uint8_t *pool, uint64_t generation,
                          uint64_t sequence_base, int timeout_ms)
{
    struct k230_rpmsg_slot_submit requests[K230_PAYLOAD_SLOT_COUNT + 1U];
    struct expected_reply expected[K230_PAYLOAD_SLOT_COUNT + 1U];
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct k230_rpmsg_protocol_header header;
    struct k230_rpmsg_slot_complete complete;
    struct timespec start, end;
    const uint32_t length = 1280U * 720U;
    uint32_t crc[K230_PAYLOAD_SLOT_COUNT];
    unsigned total = K230_PAYLOAD_SLOT_COUNT + 1U;
    unsigned i;
    unsigned busy = 0;
    int failed = 0;
    size_t reply_length;

    for (i = 0; i < K230_PAYLOAD_SLOT_COUNT; ++i) {
        uint8_t *slot = pool + (size_t)i * K230_PAYLOAD_SLOT_SIZE;

        crc[i] = fill_pattern(slot, length, padded_length(length),
                              (uint32_t)sequence_base + i);
        slot_request(&requests[i], generation, sequence_base + i, i,
                     length, crc[i], K230_PAYLOAD_FORMAT_Y8,
                     1280, 720, 1280);
        expected[i].sequence = sequence_base + i;
        expected[i].crc = crc[i];
        expected[i].slot_id = i;
        expected[i].seen = 0;
    }
    requests[K230_PAYLOAD_SLOT_COUNT] = requests[0];
    requests[K230_PAYLOAD_SLOT_COUNT].header.sequence =
        sequence_base + K230_PAYLOAD_SLOT_COUNT;
    expected[K230_PAYLOAD_SLOT_COUNT].sequence =
        sequence_base + K230_PAYLOAD_SLOT_COUNT;
    expected[K230_PAYLOAD_SLOT_COUNT].crc = crc[0];
    expected[K230_PAYLOAD_SLOT_COUNT].slot_id = 0;
    expected[K230_PAYLOAD_SLOT_COUNT].seen = 0;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < total; ++i)
        failed |= send_request(fd, &requests[i], sizeof(requests[i]));
    for (i = 0; !failed && i < total; ++i) {
        int index;

        if (receive_reply(fd, data, &reply_length, timeout_ms) ||
            check_header(data, reply_length, &header)) {
            failed = 1;
            break;
        }
        index = find_expected(expected, total, header.sequence);
        if (index < 0 || expected[index].seen) {
            fprintf(stderr, "unexpected/duplicate queue reply seq=%" PRIu64
                    "\n", header.sequence);
            failed = 1;
            break;
        }
        expected[index].seen = 1;
        if (index == (int)K230_PAYLOAD_SLOT_COUNT &&
            header.type == K230_RPMSG_MSG_ERROR &&
            header.status == K230_RPMSG_STATUS_SLOT_BUSY) {
            ++busy;
            continue;
        }
        if (check_completion(data, reply_length, &requests[index],
                             K230_RPMSG_STATUS_OK, &complete)) {
            failed = 1;
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    for (i = 0; i < total; ++i)
        if (!expected[i].seen)
            failed = 1;
    printf("%s four-slot pressure + same-slot ownership guard "
           "busy=%u serialized=%u elapsed=%.3f ms\n",
           failed ? "FAIL" : "PASS", busy, busy ? 0U : 1U,
           elapsed_ms(&start, &end));
    return failed;
}

static int drain_until_restart_reply(int fd, uint64_t restart_sequence,
                                     uint64_t generation, int timeout_ms)
{
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct k230_rpmsg_protocol_header header;
    size_t length;
    unsigned replies;

    for (replies = 0; replies <= K230_PAYLOAD_SLOT_COUNT; ++replies) {
        if (receive_reply(fd, data, &length, timeout_ms) ||
            check_header(data, length, &header))
            return 1;
        if (header.sequence == restart_sequence) {
            if (header.type != K230_RPMSG_MSG_RESTART_ENDPOINT_REPLY ||
                header.status != K230_RPMSG_STATUS_OK ||
                header.generation != generation) {
                fprintf(stderr, "bad queued-restart acknowledgement\n");
                return 1;
            }
            return 0;
        }
        if (header.generation != generation ||
            header.type != K230_RPMSG_MSG_SLOT_COMPLETE) {
            fprintf(stderr, "unexpected reply while awaiting restart\n");
            return 1;
        }
    }
    fprintf(stderr, "restart acknowledgement not received\n");
    return 1;
}

static void drain_ready(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    unsigned char data[RPMSG_MAX_PAYLOAD];

    while (poll(&pfd, 1, 20) > 0)
        if (read(fd, data, sizeof(data)) <= 0)
            break;
}

static int run_restart_test(int fd, uint8_t *pool, uint64_t old_generation,
                            uint64_t *new_generation, int timeout_ms)
{
    struct k230_rpmsg_slot_submit slots[K230_PAYLOAD_SLOT_COUNT];
    struct k230_rpmsg_slot_submit stale;
    struct k230_rpmsg_protocol_header restart;
    const uint32_t length = K230_PAYLOAD_SLOT_SIZE;
    uint64_t base = UINT64_C(300);
    unsigned i;
    int failed = 0;

    for (i = 0; i < K230_PAYLOAD_SLOT_COUNT; ++i) {
        uint8_t *slot = pool + (size_t)i * K230_PAYLOAD_SLOT_SIZE;
        uint32_t crc = fill_pattern(slot, length, length,
                                    (uint32_t)base + i);

        slot_request(&slots[i], old_generation, base + i, i, length, crc,
                     K230_PAYLOAD_FORMAT_OPAQUE, 0, 0, 0);
        failed |= send_request(fd, &slots[i], sizeof(slots[i]));
    }
    request_header(&restart, K230_RPMSG_MSG_RESTART_ENDPOINT,
                   old_generation, base + K230_PAYLOAD_SLOT_COUNT, 0,
                   sizeof(restart));
    failed |= send_request(fd, &restart, sizeof(restart));
    if (!failed)
        failed |= drain_until_restart_reply(
            fd, restart.sequence, old_generation, timeout_ms);
    drain_ready(fd);
    if (!failed)
        failed |= handshake(fd, base + 10, new_generation, timeout_ms);
    if (!failed && *new_generation == old_generation) {
        fprintf(stderr, "queued restart did not advance generation\n");
        failed = 1;
    }

    stale = slots[0];
    stale.header.sequence = base + 11;
    if (!failed)
        failed |= expect_error(fd, &stale,
                               K230_RPMSG_STATUS_STALE_GENERATION,
                               timeout_ms);
    if (!failed)
        failed |= run_one(fd, pool, *new_generation, base + 12, 0, 4096,
                          K230_PAYLOAD_FORMAT_OPAQUE, 0, 0, 0, timeout_ms);
    printf("%s queued restart recovery old-generation=%" PRIu64
           " new-generation=%" PRIu64 "\n",
           failed ? "FAIL" : "PASS", old_generation, *new_generation);
    return failed;
}

#ifdef K230_SLOT_CAMERA
struct camera_slot {
    struct k230_rpmsg_slot_submit request;
    struct timespec submitted_at;
    uint32_t busy;
};

static uint32_t copy_frame(uint8_t *destination, const uint8_t *source,
                           uint32_t data_length, uint32_t padded)
{
    uint32_t crc = UINT32_C(0xffffffff);
    uint32_t i;

    for (i = 0; i < data_length; ++i) {
        uint8_t byte = source[i];

        destination[i] = byte;
        crc = crc32_byte(crc, byte);
    }
    for (; i < padded; ++i)
        destination[i] = 0;
    __sync_synchronize();
    return ~crc;
}

static int camera_completion(int fd, struct camera_slot *slots,
                             uint32_t *outstanding, double *latencies,
                             size_t latency_capacity, size_t *latency_count,
                             int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    unsigned char data[RPMSG_MAX_PAYLOAD];
    struct k230_rpmsg_protocol_header header;
    struct k230_rpmsg_slot_complete complete;
    struct timespec now;
    size_t length;
    unsigned slot;
    int ready;
    ssize_t received;

    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready == 0)
        return 0;
    if (ready < 0) {
        fprintf(stderr, "camera completion poll failed: %s\n",
                strerror(errno));
        return -1;
    }
    received = read(fd, data, sizeof(data));
    if (received < 0) {
        fprintf(stderr, "camera completion read failed: %s\n",
                strerror(errno));
        return -1;
    }
    length = (size_t)received;
    if (check_header(data, length, &header))
        return -1;
    for (slot = 0; slot < K230_PAYLOAD_SLOT_COUNT; ++slot)
        if (slots[slot].busy &&
            slots[slot].request.header.sequence == header.sequence)
            break;
    if (slot == K230_PAYLOAD_SLOT_COUNT) {
        fprintf(stderr, "camera received unknown completion sequence=%" PRIu64
                " generation=%" PRIu64 "\n",
                header.sequence, header.generation);
        return -1;
    }
    if (check_completion(data, length, &slots[slot].request,
                         K230_RPMSG_STATUS_OK, &complete))
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (*latency_count < latency_capacity)
        latencies[(*latency_count)++] =
            elapsed_ms(&slots[slot].submitted_at, &now);
    slots[slot].busy = 0;
    if (!*outstanding) {
        fprintf(stderr, "camera completion underflow\n");
        return -1;
    }
    --*outstanding;
    return 1;
}

static int run_camera_test(int fd, uint8_t *pool, uint64_t generation,
                           unsigned seconds, unsigned video_device,
                           int timeout_ms)
{
    struct v4l2_drm_context context;
    struct v4l2_format format = { 0 };
    struct camera_slot slots[K230_PAYLOAD_SLOT_COUNT] = { 0 };
    struct timespec test_start, deadline, now;
    double *latencies;
    size_t latency_capacity = (size_t)seconds * 120U + 16U;
    size_t latency_count = 0;
    uint64_t sequence = UINT64_C(10000);
    uint32_t outstanding = 0;
    uint32_t submitted = 0;
    uint32_t completed = 0;
    uint32_t delivered = 0;
    uint32_t requeued = 0;
    uint32_t no_slot = 0;
    uint32_t capture_errors = 0;
    uint32_t submit_errors = 0;
    double max_hold_ms = 0.0;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t data_length;
    uint32_t padded;
    unsigned slot;
    int failed = 0;

    latencies = calloc(latency_capacity, sizeof(*latencies));
    if (!latencies) {
        fprintf(stderr, "camera latency allocation failed\n");
        return 1;
    }
    v4l2_drm_default_context(&context);
    context.device = video_device;
    context.display = false;
    context.width = 1280;
    context.height = 720;
    context.video_format = V4L2_PIX_FMT_NV12;
    context.buffer_num = 6;
    if (v4l2_drm_setup(&context, 1, NULL)) {
        fprintf(stderr, "camera v4l2_drm_setup failed for /dev/video%u\n",
                video_device);
        free(latencies);
        return 1;
    }
    if (v4l2_drm_start(&context)) {
        fprintf(stderr, "camera v4l2_drm_start failed\n");
        free(latencies);
        return 1;
    }
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(context.video_fd, VIDIOC_G_FMT, &format) < 0) {
        fprintf(stderr, "camera VIDIOC_G_FMT failed: %s\n", strerror(errno));
        v4l2_drm_stop(&context);
        free(latencies);
        return 1;
    }
    width = format.fmt.pix.width;
    height = format.fmt.pix.height;
    stride = format.fmt.pix.bytesperline ?
             format.fmt.pix.bytesperline : width;
    if ((uint64_t)stride * height > UINT32_MAX) {
        fprintf(stderr, "camera geometry overflows descriptor length\n");
        v4l2_drm_stop(&context);
        free(latencies);
        return 1;
    }
    data_length = stride * height;
    padded = padded_length(data_length);
    if (padded > K230_PAYLOAD_SLOT_SIZE) {
        fprintf(stderr,
                "camera Y plane does not fit slot: %ux%u stride=%u bytes=%u\n",
                width, height, stride, data_length);
        v4l2_drm_stop(&context);
        free(latencies);
        return 1;
    }

    printf("camera producer: /dev/video%u %ux%u Y8 stride=%u duration=%us "
           "buffers=%u\n", video_device, width, height, stride, seconds,
           context.buffer_num);
    clock_gettime(CLOCK_MONOTONIC, &test_start);
    deadline = test_start;
    deadline.tv_sec += seconds;
    while (1) {
        const uint8_t *source;
        struct timespec held_at;
        struct timespec released_at;
        uint32_t crc;
        int result;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec))
            break;
        do {
            result = camera_completion(fd, slots, &outstanding, latencies,
                                       latency_capacity, &latency_count, 0);
            if (result > 0)
                ++completed;
        } while (result > 0);
        if (result < 0) {
            failed = 1;
            break;
        }

        if (v4l2_drm_dump_latest(&context, 1000)) {
            ++capture_errors;
            continue;
        }
        ++delivered;
        clock_gettime(CLOCK_MONOTONIC, &held_at);
        for (slot = 0; slot < K230_PAYLOAD_SLOT_COUNT; ++slot)
            if (!slots[slot].busy)
                break;
        if (slot == K230_PAYLOAD_SLOT_COUNT) {
            if (v4l2_drm_dump_release(&context)) {
                fprintf(stderr, "camera requeue without slot failed: %s\n",
                        strerror(errno));
                ++capture_errors;
                failed = 1;
                break;
            }
            ++requeued;
            ++no_slot;
            continue;
        }

        source = (const uint8_t *)context.buffers[context.vbuffer.index].mmap;
        crc = copy_frame(pool + (size_t)slot * K230_PAYLOAD_SLOT_SIZE,
                         source, data_length, padded);
        if (v4l2_drm_dump_release(&context)) {
            fprintf(stderr, "camera requeue after copy failed: %s\n",
                    strerror(errno));
            ++capture_errors;
            failed = 1;
            break;
        }
        ++requeued;
        clock_gettime(CLOCK_MONOTONIC, &released_at);
        if (elapsed_ms(&held_at, &released_at) > max_hold_ms)
            max_hold_ms = elapsed_ms(&held_at, &released_at);

        slot_request(&slots[slot].request, generation, sequence++, slot,
                     data_length, crc, K230_PAYLOAD_FORMAT_Y8,
                     width, height, stride);
        clock_gettime(CLOCK_MONOTONIC, &slots[slot].submitted_at);
        if (send_request(fd, &slots[slot].request,
                         sizeof(slots[slot].request))) {
            ++submit_errors;
            failed = 1;
            break;
        }
        slots[slot].busy = 1;
        ++outstanding;
        ++submitted;
    }

    if (v4l2_drm_stop(&context)) {
        fprintf(stderr, "camera v4l2_drm_stop failed\n");
        failed = 1;
    }
    while (!failed && outstanding) {
        int result = camera_completion(fd, slots, &outstanding, latencies,
                                       latency_capacity, &latency_count,
                                       timeout_ms);
        if (result <= 0) {
            fprintf(stderr, "camera outstanding completion timed out\n");
            failed = 1;
            break;
        }
        ++completed;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (latency_count)
        qsort(latencies, latency_count, sizeof(*latencies), compare_double);
    printf("camera producer result: produced=%u delivered=%u requeued=%u "
           "submitted=%u completed=%u no-slot-drop=%u capture-errors=%u "
           "submit-errors=%u outstanding=%u\n",
           context.frame_count, delivered, requeued, submitted, completed,
           no_slot, capture_errors, submit_errors, outstanding);
    printf("camera producer timing: elapsed=%.3f s produced-fps=%.2f "
           "delivered-fps=%.2f max-vi-hold=%.3f ms",
           elapsed_ms(&test_start, &now) / 1000.0,
           context.frame_count * 1000.0 / elapsed_ms(&test_start, &now),
           delivered * 1000.0 / elapsed_ms(&test_start, &now), max_hold_ms);
    if (latency_count) {
        size_t p50 = (latency_count - 1U) / 2U;
        size_t p95 = (latency_count - 1U) * 95U / 100U;
        size_t p99 = (latency_count - 1U) * 99U / 100U;

        printf(" completion-ms{p50=%.3f,p95=%.3f,p99=%.3f,max=%.3f}",
               latencies[p50], latencies[p95], latencies[p99],
               latencies[latency_count - 1U]);
    }
    printf("\n");
    failed |= !delivered || delivered != requeued ||
              submitted != completed || outstanding || capture_errors ||
              submit_errors;
    printf("%s camera buffers requeued independently from remote completion\n",
           failed ? "FAIL" : "PASS");
    free(latencies);
    kd_mpi_mmz_deinit();
    return failed;
}
#endif

static int parse_number(const char *text, unsigned long *value)
{
    char *end;

    errno = 0;
    *value = strtoul(text, &end, 0);
    return errno || !*text || *end;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [device] [--loops N] [--timeout-ms N|--self-test]"
#ifdef K230_SLOT_CAMERA
            " [--camera-seconds N] [--video-device N]"
#endif
            "\n",
            name);
}

int main(int argc, char **argv)
{
    static const uint32_t sizes[] = { 64, 4096, 65536, 1280U * 720U,
                                      K230_PAYLOAD_SLOT_SIZE };
    const char *device = "/dev/rpmsg0";
    unsigned long loops = 1;
    unsigned long timeout = 3000;
#ifdef K230_SLOT_CAMERA
    unsigned long camera_seconds = 0;
    unsigned long video_device = (unsigned long)kd_mpi_get_vvcam_video00() + 1U;
#endif
    uint64_t generation = 0;
    uint64_t sequence = 10;
    uint8_t *pool = MAP_FAILED;
    int rpmsg_fd = -1;
    int mem_fd = -1;
    int failed = 0;
    int self_test = 0;
    int i;
    unsigned iteration;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--self-test")) {
            self_test = 1;
#ifdef K230_SLOT_CAMERA
        } else if (!strcmp(argv[i], "--camera-seconds") ||
                   !strcmp(argv[i], "--video-device")) {
            unsigned long *target = !strcmp(argv[i], "--camera-seconds") ?
                                    &camera_seconds : &video_device;
            if (++i >= argc || parse_number(argv[i], target) ||
                (!*target && target == &camera_seconds)) {
                usage(argv[0]);
                return 2;
            }
#endif
        } else if (!strcmp(argv[i], "--loops") ||
            !strcmp(argv[i], "--timeout-ms")) {
            unsigned long *target = !strcmp(argv[i], "--loops") ?
                                    &loops : &timeout;
            if (++i >= argc || parse_number(argv[i], target) || !*target) {
                usage(argv[0]);
                return 2;
            }
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (self_test)
        return run_self_test();

    rpmsg_fd = open(device, O_RDWR | O_CLOEXEC);
    if (rpmsg_fd < 0) {
        perror(device);
        return 1;
    }
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) {
        perror("/dev/mem");
        close(rpmsg_fd);
        return 1;
    }
    pool = mmap(NULL, K230_PAYLOAD_POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, mem_fd, K230_PAYLOAD_POOL_BASE);
    if (pool == MAP_FAILED) {
        perror("payload-pool mmap");
        close(mem_fd);
        close(rpmsg_fd);
        return 1;
    }

    printf("K230 Phase 5 payload-slot test\n");
    printf("pool=0x%" PRIx64 " slots=%u slot-size=%u cache-line=%u\n",
           K230_PAYLOAD_POOL_BASE, K230_PAYLOAD_SLOT_COUNT,
           K230_PAYLOAD_SLOT_SIZE, K230_PAYLOAD_CACHE_LINE);
    failed |= handshake(rpmsg_fd, 1, &generation, (int)timeout);
    if (!failed)
        printf("PASS payload-slot handshake generation=%" PRIu64 "\n",
               generation);
#ifdef K230_SLOT_CAMERA
    if (!failed && camera_seconds) {
        failed = run_camera_test(rpmsg_fd, pool, generation,
                                 (unsigned)camera_seconds,
                                 (unsigned)video_device, (int)timeout);
        goto out;
    }
#endif
    for (i = 0; !failed && i < (int)(sizeof(sizes) / sizeof(sizes[0])); ++i) {
        uint32_t format = sizes[i] == 1280U * 720U ?
                          K230_PAYLOAD_FORMAT_Y8 :
                          K230_PAYLOAD_FORMAT_OPAQUE;
        failed |= run_one(rpmsg_fd, pool, generation, sequence++,
                          (uint32_t)i % K230_PAYLOAD_SLOT_COUNT,
                          sizes[i], format,
                          format == K230_PAYLOAD_FORMAT_Y8 ? 1280 : 0,
                          format == K230_PAYLOAD_FORMAT_Y8 ? 720 : 0,
                          format == K230_PAYLOAD_FORMAT_Y8 ? 1280 : 0,
                          (int)timeout);
    }
    if (!failed)
        failed |= run_negative_tests(rpmsg_fd, generation, (int)timeout);
    for (iteration = 0; !failed && iteration < loops; ++iteration)
        failed |= run_queue_test(rpmsg_fd, pool, generation,
                                 UINT64_C(1000) + iteration * 10,
                                 (int)timeout);
    if (!failed)
        failed |= run_restart_test(rpmsg_fd, pool, generation, &generation,
                                   (int)timeout);

#ifdef K230_SLOT_CAMERA
out:
#endif
    munmap(pool, K230_PAYLOAD_POOL_SIZE);
    close(mem_fd);
    close(rpmsg_fd);
    printf("%s Phase 5 payload-slot checks\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
