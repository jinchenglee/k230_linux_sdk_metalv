#include <errno.h>
#include <stdint.h>
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
        "tvq_avail_idx", "tvq_consumed", "rsc_status", "driver_ok", "announced"
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

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [device] [--loops N] [--size N] [--timeout-ms N] "
            "[--sweep] [--burst]\n", name);
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
            while (poll(&pfd, 1, 0) > 0)
                read(fd, reply, size);
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
    int sweep = 0, burst = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--sweep")) {
            sweep = 1;
        } else if (!strcmp(argv[i], "--burst")) {
            burst = 1;
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
