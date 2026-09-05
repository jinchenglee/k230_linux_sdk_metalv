#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/rpmsg0";
    const char message[] = "k230-rpmsg-lite-echo";
    char reply[sizeof(message)];
    struct timespec start, end;
    struct pollfd pfd;
    ssize_t length;
    int fd;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    length = write(fd, message, sizeof(message));
    if (length != sizeof(message)) {
        if (length < 0)
            perror("write");
        else
            fprintf(stderr, "short write: %zd\n", length);
        close(fd);
        return 1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 1000) <= 0) {
        fprintf(stderr, "RPMsg echo timed out: %s\n",
                errno ? strerror(errno) : "no response");
        close(fd);
        return 1;
    }

    length = read(fd, reply, sizeof(reply));
    clock_gettime(CLOCK_MONOTONIC, &end);
    close(fd);
    if (length != sizeof(message) || memcmp(reply, message, sizeof(message))) {
        fprintf(stderr, "RPMsg echo mismatch: received %zd bytes\n", length);
        return 1;
    }

    printf("PASS bytes=%zu round_trip=%.3f ms device=%s\n",
           sizeof(message), elapsed_ms(&start, &end), device);
    return 0;
}
