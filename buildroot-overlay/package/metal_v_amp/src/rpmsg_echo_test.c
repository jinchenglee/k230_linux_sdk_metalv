#include <errno.h>
#include <stdint.h>

#include "rpmsg_protocol.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RPMSG_MAX_PAYLOAD 496U

static double elapsed_ms(const struct timespec *start,
                         const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static int number(const char *text, unsigned long *value)
{
    char *end;

    errno = 0;
    *value = strtoul(text, &end, 0);
    return errno || *end != '\0' || !*text;
}

static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a > b ? 1 : a < b ? -1 : 0;
}

static double percentile(const double *sorted, unsigned long count,
                         double fraction)
{
    unsigned long index = (unsigned long)(fraction * (count - 1));
    return sorted[index];
}

/*
 * Queue-full probe: push `loops` messages without reading any reply, so the
 * firmware's RL_DONT_BLOCK send path runs out of TX buffers, then drain and
 * count what actually came back. Silent loss here means the echo path drops
 * on a full queue rather than applying back-pressure.
 */
static int run_burst(const char *device, unsigned long loops,
                     unsigned long size, int timeout_ms)
{
    unsigned char *message = malloc(size);
    unsigned char *reply = malloc(size);
    struct pollfd pfd;
    unsigned long i, sent = 0, drained = 0;
    int fd = open(device, O_RDWR | O_CLOEXEC);

    if (fd < 0 || !message || !reply) {
        perror(device);
        return 1;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    memset(message, 0xa5, size);
    for (i = 0; i < loops; ++i) {
        if (write(fd, message, size) != (ssize_t)size)
            break;
        ++sent;
    }
    while (poll(&pfd, 1, timeout_ms) > 0 && read(fd, reply, size) > 0)
        ++drained;
    printf("%s BURST size=%lu sent=%lu drained=%lu lost=%lu\n",
           sent == drained ? "PASS" : "FAIL", size, sent, drained,
           sent - drained);
    close(fd);
    free(message);
    free(reply);
    return sent == drained ? 0 : 1;
}

#define AMP_SHM_PHYS_BASE 0x1d000000UL
#define STATS_OFFSET      0x0200U
#define STATS_MAGIC       0x52535431U

/*
 * The big-core UART drops characters under sustained output, so the firmware
 * publishes its counters into the AMP shm page instead. Read them here.
 */
static int show_stats(void)
{
    static const char *names[] = {
        "magic", "link_up", "rx_callbacks", "tx_sent", "tx_failed",
        "fetch_rx", "fetch_tx", "rvq_avail_idx", "rvq_consumed",
        "tvq_avail_idx", "tvq_consumed", "rsc_status", "driver_ok", "announced",
        "generation_lo", "generation_hi", "hellos", "protocol_msgs",
        "rejected_ver", "rejected_caps", "rejected_gen", "endpoint_restarts",
        "restart_failures"
    };
    volatile uint32_t *stats;
    void *map;
    unsigned i;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               AMP_SHM_PHYS_BASE);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    stats = (volatile uint32_t *)((char *)map + STATS_OFFSET);
    if (stats[0] != STATS_MAGIC) {
        printf("stats block not published (magic=0x%08x)\n", stats[0]);
        munmap(map, 0x1000);
        close(fd);
        return 1;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        printf("%-14s %u\n", names[i], stats[i]);
    munmap(map, 0x1000);
    close(fd);
    return 0;
}

static void protocol_request_init(struct k230_rpmsg_protocol_header *request,
                                  uint16_t version, uint16_t type,
                                  uint64_t generation, uint64_t sequence,
                                  uint64_t capabilities, uint32_t message_size)
{
    memset(request, 0, sizeof(*request));
    request->magic = K230_RPMSG_PROTOCOL_MAGIC;
    request->version = version;
    request->header_size = K230_RPMSG_PROTOCOL_HEADER_SIZE;
    request->type = type;
    request->message_size = message_size;
    request->generation = generation;
    request->sequence = sequence;
    request->capabilities = capabilities;
}

static int protocol_exchange(int fd, const void *request, size_t request_len,
                             unsigned char *reply, size_t *reply_len,
                             int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    ssize_t written;
    ssize_t received;
    int ready;

    written = write(fd, request, request_len);
    if (written != (ssize_t)request_len) {
        fprintf(stderr, "protocol write failed: %s\n",
                written < 0 ? strerror(errno) : "short write");
        return 1;
    }
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        fprintf(stderr, "protocol reply %s\n",
                ready == 0 ? "timed out" : strerror(errno));
        return 1;
    }
    received = read(fd, reply, RPMSG_MAX_PAYLOAD);
    if (received < 0) {
        fprintf(stderr, "protocol read failed: %s\n", strerror(errno));
        return 1;
    }
    *reply_len = (size_t)received;
    return 0;
}

static int protocol_reply_check(
    const unsigned char *reply_data, size_t reply_len, uint64_t sequence,
    uint16_t expected_type, uint16_t expected_status,
    struct k230_rpmsg_protocol_header *reply)
{
    if (reply_len < sizeof(*reply)) {
        fprintf(stderr, "protocol reply is too short: %zu\n", reply_len);
        return 1;
    }
    memcpy(reply, reply_data, sizeof(*reply));
    if (reply->magic != K230_RPMSG_PROTOCOL_MAGIC ||
        reply->version != K230_RPMSG_PROTOCOL_VERSION ||
        reply->header_size != sizeof(*reply) ||
        reply->message_size != reply_len ||
        reply->sequence != sequence ||
        reply->type != expected_type ||
        reply->status != expected_status) {
        fprintf(stderr,
                "bad protocol reply: magic=%08x version=%u header=%u "
                "type=%u status=%u size=%u/%zu sequence=%llu/%llu\n",
                reply->magic, reply->version, reply->header_size,
                reply->type, reply->status, reply->message_size, reply_len,
                (unsigned long long)reply->sequence,
                (unsigned long long)sequence);
        return 1;
    }
    return 0;
}

static int protocol_handshake(int fd, int timeout_ms, uint64_t sequence,
                              uint64_t *generation)
{
    struct k230_rpmsg_protocol_header request;
    struct k230_rpmsg_protocol_header reply;
    unsigned char reply_data[RPMSG_MAX_PAYLOAD];
    size_t reply_len;

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_HELLO, 0, sequence,
                          K230_RPMSG_REQUIRED_CAPABILITIES, sizeof(request));
    if (protocol_exchange(fd, &request, sizeof(request), reply_data,
                          &reply_len, timeout_ms) ||
        protocol_reply_check(reply_data, reply_len, sequence,
                             K230_RPMSG_MSG_HELLO_REPLY,
                             K230_RPMSG_STATUS_OK, &reply))
        return 1;
    if (!reply.generation ||
        (reply.capabilities & K230_RPMSG_REQUIRED_CAPABILITIES) !=
            K230_RPMSG_REQUIRED_CAPABILITIES) {
        fprintf(stderr, "handshake lacks generation or required capabilities\n");
        return 1;
    }
    *generation = reply.generation;
    return 0;
}

static int run_protocol_test(const char *device, int timeout_ms)
{
    static const char body[] = "phase4-generation";
    struct k230_rpmsg_protocol_header request;
    struct k230_rpmsg_protocol_header reply;
    unsigned char request_data[RPMSG_MAX_PAYLOAD];
    unsigned char reply_data[RPMSG_MAX_PAYLOAD];
    uint64_t generation = 0;
    size_t message_len;
    size_t reply_len;
    int fd = open(device, O_RDWR | O_CLOEXEC);
    int failed = 0;

    if (fd < 0) {
        perror(device);
        return 1;
    }
    if (protocol_handshake(fd, timeout_ms, 1, &generation)) {
        failed = 1;
        goto out;
    }

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION + 1,
                          K230_RPMSG_MSG_HELLO, 0, 2,
                          K230_RPMSG_REQUIRED_CAPABILITIES, sizeof(request));
    failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 2, K230_RPMSG_MSG_ERROR,
            K230_RPMSG_STATUS_BAD_VERSION, &reply);

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_HELLO, 0, 3,
                          UINT64_C(1) << 63, sizeof(request));
    if (!failed)
        failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                    &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 3, K230_RPMSG_MSG_ERROR,
            K230_RPMSG_STATUS_UNSUPPORTED_CAPABILITY, &reply);

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_ECHO, generation ^ UINT64_C(1), 4,
                          0, sizeof(request));
    if (!failed)
        failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                    &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 4, K230_RPMSG_MSG_ERROR,
            K230_RPMSG_STATUS_STALE_GENERATION, &reply);
    if (!failed && reply.generation != generation) {
        fprintf(stderr, "stale-generation reply did not advertise current generation\n");
        failed = 1;
    }

    message_len = sizeof(request) + sizeof(body);
    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_ECHO, generation, 5, 0, message_len);
    memcpy(request_data, &request, sizeof(request));
    memcpy(request_data + sizeof(request), body, sizeof(body));
    if (!failed)
        failed |= protocol_exchange(fd, request_data, message_len, reply_data,
                                    &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 5, K230_RPMSG_MSG_ECHO_REPLY,
            K230_RPMSG_STATUS_OK, &reply);
    if (!failed &&
        (reply.generation != generation ||
         reply_len != message_len ||
         memcmp(reply_data + sizeof(reply), body, sizeof(body)))) {
        fprintf(stderr, "protocol echo body mismatch\n");
        failed = 1;
    }

out:
    close(fd);
    printf("%s protocol handshake/version/capability/generation checks"
           " generation=%llu\n",
           failed ? "FAIL" : "PASS", (unsigned long long)generation);
    return failed;
}

static int run_endpoint_restart_test(const char *device, int timeout_ms)
{
    struct k230_rpmsg_protocol_header request;
    struct k230_rpmsg_protocol_header reply;
    unsigned char reply_data[RPMSG_MAX_PAYLOAD];
    uint64_t old_generation = 0;
    uint64_t new_generation = 0;
    size_t reply_len;
    int fd = open(device, O_RDWR | O_CLOEXEC);
    int failed = 0;

    if (fd < 0) {
        perror(device);
        return 1;
    }
    if (protocol_handshake(fd, timeout_ms, 10, &old_generation)) {
        failed = 1;
        goto out;
    }

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_RESTART_ENDPOINT, old_generation, 11,
                          0, sizeof(request));
    failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 11,
            K230_RPMSG_MSG_RESTART_ENDPOINT_REPLY,
            K230_RPMSG_STATUS_OK, &reply);
    if (!failed && reply.generation != old_generation) {
        fprintf(stderr, "endpoint restart acknowledgement generation mismatch\n");
        failed = 1;
    }
    if (!failed)
        failed |= protocol_handshake(fd, timeout_ms, 12, &new_generation);
    if (!failed && new_generation == old_generation) {
        fprintf(stderr, "endpoint restart did not advance generation\n");
        failed = 1;
    }

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_ECHO, old_generation, 13,
                          0, sizeof(request));
    if (!failed)
        failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                    &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 13, K230_RPMSG_MSG_ERROR,
            K230_RPMSG_STATUS_STALE_GENERATION, &reply);
    if (!failed && reply.generation != new_generation) {
        fprintf(stderr, "post-restart stale reply generation mismatch\n");
        failed = 1;
    }

    protocol_request_init(&request, K230_RPMSG_PROTOCOL_VERSION,
                          K230_RPMSG_MSG_ECHO, new_generation, 14,
                          0, sizeof(request));
    if (!failed)
        failed |= protocol_exchange(fd, &request, sizeof(request), reply_data,
                                    &reply_len, timeout_ms);
    if (!failed)
        failed |= protocol_reply_check(
            reply_data, reply_len, 14, K230_RPMSG_MSG_ECHO_REPLY,
            K230_RPMSG_STATUS_OK, &reply);
    if (!failed && reply.generation != new_generation) {
        fprintf(stderr, "post-restart echo generation mismatch\n");
        failed = 1;
    }

out:
    close(fd);
    printf("%s endpoint restart old-generation=%llu new-generation=%llu\n",
           failed ? "FAIL" : "PASS",
           (unsigned long long)old_generation,
           (unsigned long long)new_generation);
    return failed;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [device] [--loops N] [--size N] [--timeout-ms N] "
            "[--sweep|--burst|--protocol|--restart]\n", name);
}

static int run_test(const char *device, unsigned long loops,
                    unsigned long size, int timeout_ms)
{
    unsigned char *message;
    unsigned char *reply;
    double *samples;
    struct pollfd pfd;
    unsigned long i, j, completed = 0, timeouts = 0, mismatches = 0;
    long first_loss = -1, last_loss = -1;
    double total = 0.0;
    int fd = -1;

    message = malloc(size);
    reply = malloc(size);
    samples = malloc(loops * sizeof(*samples));
    if (!message || !reply || !samples) {
        fprintf(stderr, "allocation failed for size=%lu loops=%lu\n",
                size, loops);
        free(message);
        free(reply);
        free(samples);
        return 1;
    }

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        free(message);
        free(reply);
        free(samples);
        return 1;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;

    for (i = 0; i < loops; ++i) {
        struct timespec start, end;
        ssize_t written, received;

        for (j = 0; j < size; ++j)
            message[j] = (unsigned char)((i * 131U + j * 17U + 0x5aU) & 0xffU);
        clock_gettime(CLOCK_MONOTONIC, &start);
        written = write(fd, message, size);
        if (written != (ssize_t)size) {
            if (written < 0)
                fprintf(stderr, "write failed at loop=%lu: %s\n", i,
                        strerror(errno));
            else
                fprintf(stderr, "short write at loop=%lu: %zd/%lu\n", i,
                        written, size);
            close(fd);
            free(message);
            free(reply);
            free(samples);
            return 1;
        }
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            /*
             * Keep going rather than aborting the run: whether the link
             * recovers on the next message or is stalled for good is the
             * whole diagnostic, and one lost message must not hide it.
             */
            ++timeouts;
            if (first_loss == -1)
                first_loss = (long)i;
            last_loss = (long)i;
            continue;
        }
        received = read(fd, reply, size);
        clock_gettime(CLOCK_MONOTONIC, &end);
        if (received != (ssize_t)size || memcmp(reply, message, size)) {
            ++mismatches;
            if (first_loss == -1)
                first_loss = (long)i;
            last_loss = (long)i;
            /* A late reply to an earlier loop would desync every following
             * iteration; drop anything already queued before continuing. */
            while (poll(&pfd, 1, 0) > 0) {
                ssize_t discarded = read(fd, reply, size);
                if (discarded <= 0)
                    break;
            }
            continue;
        }
        samples[completed++] = elapsed_ms(&start, &end);
        total += samples[completed - 1];
    }
    close(fd);

    if (!completed) {
        printf("FAIL size=%lu loops=%lu ok=0 timeouts=%lu mismatches=%lu "
               "first-loss=%ld last-loss=%ld\n", size, loops, timeouts,
               mismatches, first_loss, last_loss);
        free(message);
        free(reply);
        free(samples);
        return 1;
    }

    qsort(samples, completed, sizeof(*samples), compare_double);
    printf("%s size=%lu loops=%lu ok=%lu timeouts=%lu mismatches=%lu "
           "first-loss=%ld last-loss=%ld min=%.3f ms avg=%.3f ms p50=%.3f ms p95=%.3f ms "
           "p99=%.3f ms max=%.3f ms rate=%.1f msg/s\n",
           (timeouts || mismatches) ? "FAIL" : "PASS", size, loops, completed,
           timeouts, mismatches, first_loss, last_loss, samples[0],
           total / completed,
           percentile(samples, completed, 0.50),
           percentile(samples, completed, 0.95),
           percentile(samples, completed, 0.99), samples[completed - 1],
           1000.0 / (total / completed));
    free(message);
    free(reply);
    free(samples);
    return timeouts || mismatches ? 1 : 0;
}

int main(int argc, char **argv)
{
    static const unsigned long sweep_sizes[] = { 1, 16, 64, 128, 256, 496 };
    const char *device = "/dev/rpmsg0";
    unsigned long loops = 1, size = 21, timeout = 1000;
    int sweep = 0, burst = 0, protocol = 0, restart = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--sweep")) {
            sweep = 1;
        } else if (!strcmp(argv[i], "--burst")) {
            burst = 1;
        } else if (!strcmp(argv[i], "--protocol")) {
            protocol = 1;
        } else if (!strcmp(argv[i], "--restart")) {
            restart = 1;
        } else if (!strcmp(argv[i], "--stats")) {
            return show_stats();
        } else if (!strcmp(argv[i], "--loops") || !strcmp(argv[i], "--size") ||
                   !strcmp(argv[i], "--timeout-ms")) {
            unsigned long *target;
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            target = !strcmp(argv[i], "--loops") ? &loops :
                     !strcmp(argv[i], "--size") ? &size : &timeout;
            if (number(argv[++i], target) || !*target) {
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
    if (size > RPMSG_MAX_PAYLOAD) {
        fprintf(stderr, "size must be 1..%u bytes\n", RPMSG_MAX_PAYLOAD);
        return 2;
    }
    if (sweep + burst + protocol + restart > 1) {
        usage(argv[0]);
        return 2;
    }
    if (protocol)
        return run_protocol_test(device, (int)timeout);
    if (restart)
        return run_endpoint_restart_test(device, (int)timeout);
    if (burst)
        return run_burst(device, loops, size, (int)timeout);
    if (sweep) {
        int status = 0;
        for (i = 0; i < (int)(sizeof(sweep_sizes) / sizeof(sweep_sizes[0])); ++i)
            status |= run_test(device, loops, sweep_sizes[i], (int)timeout);
        return status;
    }
    return run_test(device, loops, size, (int)timeout);
}
