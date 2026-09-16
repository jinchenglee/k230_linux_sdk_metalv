#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <amp_shared_buffer_pool.h>

#define PROBE_BUFFER_COUNT 6U

struct capture_buffer {
    int fd;
    uint64_t remote_token;
    uint64_t capacity;
};

static double elapsed_ms(const struct timespec *start,
                         const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static int parse_unsigned(const char *text, unsigned *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || !*text || *end || parsed > UINT32_MAX)
        return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int queue_buffer(int video_fd, unsigned index,
                        const struct capture_buffer *capture)
{
    struct v4l2_buffer buffer = { 0 };

    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = index;
    buffer.m.fd = capture->fd;
    buffer.length = (uint32_t)capture->capacity;
    return ioctl(video_fd, VIDIOC_QBUF, &buffer);
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [--video-device N] [--frames N] [--pool PATH]\n",
            name);
}

int main(int argc, char **argv)
{
    const char *pool_path = "/dev/amp-shared-buffer-pool";
    struct amp_shared_buffer_pool_info pool_info = { 0 };
    struct capture_buffer captures[PROBE_BUFFER_COUNT];
    struct v4l2_requestbuffers request = { 0 };
    struct v4l2_format format = { 0 };
    struct timespec started, finished;
    unsigned video_device = 2;
    unsigned frames = 30;
    unsigned captured = 0;
    unsigned seen_mask = 0;
    unsigned exported = 0;
    unsigned queued = 0;
    unsigned i;
    int pool_fd = -1;
    int video_fd = -1;
    int streaming = 0;
    int failed = 0;
    char video_path[64];

    for (i = 0; i < PROBE_BUFFER_COUNT; ++i)
        captures[i].fd = -1;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--pool")) {
            if (++i >= (unsigned)argc) {
                usage(argv[0]);
                return 2;
            }
            pool_path = argv[i];
        } else if (!strcmp(argv[i], "--video-device") ||
                   !strcmp(argv[i], "--frames")) {
            unsigned *target = !strcmp(argv[i], "--video-device") ?
                               &video_device : &frames;

            if (++i >= (unsigned)argc || parse_unsigned(argv[i], target) ||
                (target == &frames && !*target)) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    pool_fd = open(pool_path, O_RDONLY | O_CLOEXEC);
    if (pool_fd < 0) {
        fprintf(stderr, "%s: %s\n", pool_path, strerror(errno));
        return 1;
    }
    if (ioctl(pool_fd, AMP_SHARED_BUFFER_IOC_GET_INFO, &pool_info) < 0) {
        fprintf(stderr, "camera-pool GET_INFO failed: %s\n", strerror(errno));
        failed = 1;
        goto out;
    }
    if (pool_info.version != AMP_SHARED_BUFFER_POOL_ABI_VERSION ||
        !(pool_info.capabilities & AMP_SHARED_BUFFER_CAP_CONTIGUOUS)) {
        fprintf(stderr, "shared-pool ABI/capability mismatch: version=%u caps=0x%x\n",
                pool_info.version, pool_info.capabilities);
        failed = 1;
        goto out;
    }

    snprintf(video_path, sizeof(video_path), "/dev/video%u", video_device);
    video_fd = open(video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video_fd < 0) {
        fprintf(stderr, "%s: %s\n", video_path, strerror(errno));
        failed = 1;
        goto out;
    }
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 1280;
    format.fmt.pix.height = 720;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(video_fd, VIDIOC_S_FMT, &format) < 0) {
        fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
        failed = 1;
        goto out;
    }
    if (pool_info.pool_size < format.fmt.pix.sizeimage * PROBE_BUFFER_COUNT) {
        fprintf(stderr, "pool too small: capacity=%" PRIu64
                " requested=%" PRIu64 "\n", pool_info.pool_size,
                (uint64_t)format.fmt.pix.sizeimage * PROBE_BUFFER_COUNT);
        failed = 1;
        goto out;
    }

    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = PROBE_BUFFER_COUNT;
    if (ioctl(video_fd, VIDIOC_REQBUFS, &request) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS(DMABUF) failed: %s\n",
                strerror(errno));
        failed = 1;
        goto out;
    }
    if (request.count < PROBE_BUFFER_COUNT) {
        fprintf(stderr, "V4L2 returned only %u buffer slots\n", request.count);
        failed = 1;
        goto out;
    }

    printf("AMP shared-pool V4L2 DMA-BUF probe\n");
    printf("pool=%s remote-base=0x%" PRIx64 " size=%" PRIu64
           " alignment=%" PRIu64 " buffers=%u\n",
           pool_path, pool_info.remote_base, pool_info.pool_size,
           pool_info.minimum_alignment, PROBE_BUFFER_COUNT);
    printf("camera=%s format=%c%c%c%c geometry=%ux%u stride=%u "
           "sizeimage=%u\n", video_path,
           format.fmt.pix.pixelformat & 0xff,
           (format.fmt.pix.pixelformat >> 8) & 0xff,
           (format.fmt.pix.pixelformat >> 16) & 0xff,
           (format.fmt.pix.pixelformat >> 24) & 0xff,
           format.fmt.pix.width, format.fmt.pix.height,
           format.fmt.pix.bytesperline, format.fmt.pix.sizeimage);

    for (i = 0; i < PROBE_BUFFER_COUNT; ++i) {
        struct amp_shared_buffer_alloc allocation = {
            .capacity = format.fmt.pix.sizeimage,
            .alignment = pool_info.minimum_alignment,
        };

        if (ioctl(pool_fd, AMP_SHARED_BUFFER_IOC_ALLOC, &allocation) < 0) {
            fprintf(stderr, "ALLOC[%u] failed: %s\n", i,
                    strerror(errno));
            failed = 1;
            goto out;
        }
        captures[i].fd = allocation.fd;
        captures[i].remote_token = allocation.remote_token;
        captures[i].capacity = allocation.capacity;
        ++exported;
        printf("buffer[%u]: dmabuf-fd=%d remote-token=0x%" PRIx64
               " capacity=%" PRIu64 " allocation=%" PRIu64 "\n",
               i, allocation.fd, allocation.remote_token,
               allocation.capacity, allocation.allocation_id);
        if (queue_buffer(video_fd, i, &captures[i]) < 0) {
            fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n", i,
                    strerror(errno));
            failed = 1;
            goto out;
        }
        ++queued;
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (ioctl(video_fd, VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
            failed = 1;
            goto out;
        }
    }
    streaming = 1;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (captured < frames) {
        struct pollfd poll_fd = { .fd = video_fd, .events = POLLIN | POLLPRI };
        struct v4l2_buffer buffer = { 0 };
        int ready;

        do {
            ready = poll(&poll_fd, 1, 2000);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) {
            fprintf(stderr, "camera poll failed: %s\n",
                    ready ? strerror(errno) : "timeout");
            failed = 1;
            break;
        }
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_DMABUF;
        if (ioctl(video_fd, VIDIOC_DQBUF, &buffer) < 0) {
            fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        if (buffer.index >= PROBE_BUFFER_COUNT) {
            fprintf(stderr, "invalid dequeued index %u\n", buffer.index);
            failed = 1;
            break;
        }
        seen_mask |= 1U << buffer.index;
        if (queue_buffer(video_fd, buffer.index,
                         &captures[buffer.index]) < 0) {
            fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n", buffer.index,
                    strerror(errno));
            failed = 1;
            break;
        }
        ++captured;
    }
    clock_gettime(CLOCK_MONOTONIC, &finished);
    printf("capture: completed=%u elapsed=%.3f s fps=%.2f "
           "seen-mask=0x%x copies=0\n", captured,
           elapsed_ms(&started, &finished) / 1000.0,
           captured * 1000.0 / elapsed_ms(&started, &finished), seen_mask);

out:
    if (streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (ioctl(video_fd, VIDIOC_STREAMOFF, &type) < 0) {
            fprintf(stderr, "VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
            failed = 1;
        }
    }
    if (video_fd >= 0 && request.count) {
        request.count = 0;
        if (ioctl(video_fd, VIDIOC_REQBUFS, &request) < 0) {
            fprintf(stderr, "VIDIOC_REQBUFS release failed: %s\n",
                    strerror(errno));
            failed = 1;
        }
    }
    if (video_fd >= 0)
        close(video_fd);
    for (i = 0; i < exported; ++i)
        close(captures[i].fd);
    if (pool_fd >= 0)
        close(pool_fd);
    printf("%s shared-pool DMA-BUF capture: exported=%u queued=%u "
           "captured=%u\n", failed ? "FAIL" : "PASS", exported, queued,
           captured);
    return failed ? 1 : 0;
}
