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

#include <k230_amp_camera_pool.h>

struct capture_buffer {
    int fd;
    uint64_t physical;
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
    const char *pool_path = "/dev/k230-amp-camera-pool";
    struct k230_amp_camera_pool_info pool_info = { 0 };
    struct capture_buffer captures[K230_AMP_CAMERA_BUFFER_COUNT];
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

    for (i = 0; i < K230_AMP_CAMERA_BUFFER_COUNT; ++i)
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
    if (ioctl(pool_fd, K230_AMP_CAMERA_IOC_GET_INFO, &pool_info) < 0) {
        fprintf(stderr, "camera-pool GET_INFO failed: %s\n", strerror(errno));
        failed = 1;
        goto out;
    }
    if (pool_info.version != K230_AMP_CAMERA_POOL_ABI_VERSION ||
        pool_info.buffer_count < K230_AMP_CAMERA_BUFFER_COUNT) {
        fprintf(stderr, "camera-pool ABI/count mismatch: version=%u count=%u\n",
                pool_info.version, pool_info.buffer_count);
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
    if (pool_info.buffer_size < format.fmt.pix.sizeimage) {
        fprintf(stderr, "pool buffer too small: capacity=%" PRIu64
                " sizeimage=%u\n", pool_info.buffer_size,
                format.fmt.pix.sizeimage);
        failed = 1;
        goto out;
    }

    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = K230_AMP_CAMERA_BUFFER_COUNT;
    if (ioctl(video_fd, VIDIOC_REQBUFS, &request) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS(DMABUF) failed: %s\n",
                strerror(errno));
        failed = 1;
        goto out;
    }
    if (request.count < K230_AMP_CAMERA_BUFFER_COUNT) {
        fprintf(stderr, "V4L2 returned only %u buffer slots\n", request.count);
        failed = 1;
        goto out;
    }

    printf("K230 fixed-pool V4L2 DMA-BUF probe\n");
    printf("pool=%s base=0x%" PRIx64 " size=%" PRIu64
           " buffers=%u buffer-size=%" PRIu64 "\n",
           pool_path, pool_info.physical_base, pool_info.pool_size,
           pool_info.buffer_count, pool_info.buffer_size);
    printf("camera=%s format=%c%c%c%c geometry=%ux%u stride=%u "
           "sizeimage=%u\n", video_path,
           format.fmt.pix.pixelformat & 0xff,
           (format.fmt.pix.pixelformat >> 8) & 0xff,
           (format.fmt.pix.pixelformat >> 16) & 0xff,
           (format.fmt.pix.pixelformat >> 24) & 0xff,
           format.fmt.pix.width, format.fmt.pix.height,
           format.fmt.pix.bytesperline, format.fmt.pix.sizeimage);

    for (i = 0; i < K230_AMP_CAMERA_BUFFER_COUNT; ++i) {
        struct k230_amp_camera_pool_buffer get = { .id = i };

        if (ioctl(pool_fd, K230_AMP_CAMERA_IOC_GET_BUFFER, &get) < 0) {
            fprintf(stderr, "GET_BUFFER[%u] failed: %s\n", i,
                    strerror(errno));
            failed = 1;
            goto out;
        }
        captures[i].fd = get.fd;
        captures[i].physical = get.physical;
        captures[i].capacity = get.capacity;
        ++exported;
        printf("buffer[%u]: dmabuf-fd=%d phys=0x%" PRIx64
               " capacity=%" PRIu64 "\n", i, get.fd, get.physical,
               get.capacity);
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
        if (buffer.index >= K230_AMP_CAMERA_BUFFER_COUNT) {
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
    printf("%s fixed-pool DMA-BUF capture: exported=%u queued=%u "
           "captured=%u\n", failed ? "FAIL" : "PASS", exported, queued,
           captured);
    return failed ? 1 : 0;
}
