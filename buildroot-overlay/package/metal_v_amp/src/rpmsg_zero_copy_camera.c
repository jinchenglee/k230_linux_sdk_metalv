#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <k230_amp_camera_pool.h>

#include "rpmsg_protocol.h"

#define RPMSG_MAX_PAYLOAD 496U
#define BIG_CORE_HZ 1600000000.0

enum buffer_state {
    BUFFER_EXPORTED,
    BUFFER_VI_QUEUED,
    BUFFER_PENDING,
    BUFFER_REMOTE,
};

struct capture_buffer {
    int fd;
    uint64_t physical;
    uint64_t capacity;
    uint8_t *mapping;
    enum buffer_state state;
    uint64_t sequence;
    struct timespec submitted_at;
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

static uint32_t padded_length(uint32_t length)
{
    return (length + K230_PAYLOAD_CACHE_LINE - 1U) &
           ~(K230_PAYLOAD_CACHE_LINE - 1U);
}

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    unsigned bit;

    crc ^= byte;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^
              (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    return crc;
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
    return -1;
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
        return -1;
    }
    received = read(fd, reply, RPMSG_MAX_PAYLOAD);
    if (received < 0) {
        fprintf(stderr, "RPMsg read failed: %s\n", strerror(errno));
        return -1;
    }
    *length = (size_t)received;
    return 0;
}

static int check_header(const unsigned char *data, size_t length,
                        struct k230_rpmsg_protocol_header *header)
{
    if (length < sizeof(*header)) {
        fprintf(stderr, "short protocol reply: %zu\n", length);
        return -1;
    }
    memcpy(header, data, sizeof(*header));
    if (header->magic != K230_RPMSG_PROTOCOL_MAGIC ||
        header->version != K230_RPMSG_PROTOCOL_VERSION ||
        header->header_size != sizeof(*header) ||
        header->message_size != length) {
        fprintf(stderr, "bad protocol reply header\n");
        return -1;
    }
    return 0;
}

static int handshake(int fd, uint64_t sequence, uint64_t *generation,
                     int timeout_ms)
{
    struct k230_rpmsg_protocol_header request;
    struct k230_rpmsg_protocol_header reply;
    unsigned char data[RPMSG_MAX_PAYLOAD];
    const uint64_t required = K230_RPMSG_REQUIRED_CAPABILITIES |
                              K230_RPMSG_CAP_CAMERA_BUFFERS;
    size_t length;

    request_header(&request, K230_RPMSG_MSG_HELLO, 0, sequence, required,
                   sizeof(request));
    if (send_request(fd, &request, sizeof(request)) ||
        receive_reply(fd, data, &length, timeout_ms) ||
        check_header(data, length, &reply))
        return -1;
    if (reply.sequence != sequence ||
        reply.type != K230_RPMSG_MSG_HELLO_REPLY ||
        reply.status != K230_RPMSG_STATUS_OK || !reply.generation ||
        (reply.capabilities & required) != required) {
        fprintf(stderr, "camera-buffer capability handshake failed\n");
        return -1;
    }
    *generation = reply.generation;
    return 0;
}

static int register_buffer(int rpmsg_fd, uint64_t generation,
                           uint64_t sequence, unsigned id,
                           const struct capture_buffer *buffer,
                           int timeout_ms)
{
    struct k230_rpmsg_camera_register request = { 0 };
    struct k230_rpmsg_camera_register reply;
    struct k230_rpmsg_protocol_header header;
    unsigned char data[RPMSG_MAX_PAYLOAD];
    size_t length;

    request_header(&request.header, K230_RPMSG_MSG_CAMERA_REGISTER,
                   generation, sequence, 0, sizeof(request));
    request.buffer_id = id;
    request.physical = buffer->physical;
    request.capacity = buffer->capacity;
    if (send_request(rpmsg_fd, &request, sizeof(request)) ||
        receive_reply(rpmsg_fd, data, &length, timeout_ms) ||
        check_header(data, length, &header))
        return -1;
    if (length != sizeof(reply) ||
        header.type != K230_RPMSG_MSG_CAMERA_REGISTER_REPLY ||
        header.status != K230_RPMSG_STATUS_OK ||
        header.sequence != sequence || header.generation != generation) {
        fprintf(stderr,
                "buffer[%u] registration rejected type=%u status=%u\n",
                id, header.type, header.status);
        return -1;
    }
    memcpy(&reply, data, sizeof(reply));
    if (reply.buffer_id != id || reply.physical != buffer->physical ||
        reply.capacity != buffer->capacity) {
        fprintf(stderr, "buffer[%u] registration reply mismatch\n", id);
        return -1;
    }
    return 0;
}

static int queue_buffer(int video_fd, unsigned index,
                        struct capture_buffer *capture)
{
    struct v4l2_buffer buffer = { 0 };

    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = index;
    buffer.m.fd = capture->fd;
    buffer.length = (uint32_t)capture->capacity;
    if (ioctl(video_fd, VIDIOC_QBUF, &buffer) < 0)
        return -1;
    capture->state = BUFFER_VI_QUEUED;
    return 0;
}

static int submit_buffer(int rpmsg_fd, uint64_t generation,
                         uint64_t sequence, unsigned id,
                         struct capture_buffer *buffer, uint32_t data_length,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    struct k230_rpmsg_camera_submit request = { 0 };

    request_header(&request.header, K230_RPMSG_MSG_CAMERA_SUBMIT,
                   generation, sequence, 0, sizeof(request));
    request.buffer_id = id;
    request.data_length = data_length;
    request.padded_length = padded_length(data_length);
    request.format = K230_PAYLOAD_FORMAT_Y8;
    request.width = width;
    request.height = height;
    request.stride = stride;
    /* Publish ownership only after observing the device-completion event. */
    __sync_synchronize();
    if (send_request(rpmsg_fd, &request, sizeof(request)))
        return -1;
    buffer->sequence = sequence;
    buffer->state = BUFFER_REMOTE;
    clock_gettime(CLOCK_MONOTONIC, &buffer->submitted_at);
    return 0;
}

static int diagnostic_crc(struct capture_buffer *buffer, uint32_t length,
                          uint32_t expected, uint32_t *observed)
{
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
    };
    int failed = 0;

    if (ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        fprintf(stderr, "DMA_BUF_SYNC START failed: %s\n", strerror(errno));
        return -1;
    }
    *observed = crc32(buffer->mapping, length);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        fprintf(stderr, "DMA_BUF_SYNC END failed: %s\n", strerror(errno));
        failed = -1;
    }
    if (*observed != expected)
        failed = 1;
    return failed;
}

static int read_completion(int rpmsg_fd, uint64_t generation,
                           int remote, struct capture_buffer *buffers,
                           struct k230_rpmsg_camera_complete *completion)
{
    struct k230_rpmsg_protocol_header header;
    unsigned char data[RPMSG_MAX_PAYLOAD];
    ssize_t received = read(rpmsg_fd, data, sizeof(data));

    if (received < 0) {
        fprintf(stderr, "RPMsg completion read failed: %s\n",
                strerror(errno));
        return -1;
    }
    if (check_header(data, (size_t)received, &header))
        return -1;
    if (remote < 0 || (size_t)received != sizeof(*completion) ||
        header.type != K230_RPMSG_MSG_CAMERA_COMPLETE ||
        header.status != K230_RPMSG_STATUS_OK ||
        header.generation != generation ||
        header.sequence != buffers[remote].sequence) {
        fprintf(stderr,
                "unexpected camera completion type=%u status=%u "
                "generation=%" PRIu64 " sequence=%" PRIu64 "\n",
                header.type, header.status, header.generation,
                header.sequence);
        return -1;
    }
    memcpy(completion, data, sizeof(*completion));
    if (completion->buffer_id != (unsigned)remote)
        return -1;
    return 0;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [--seconds N] [--timeout-ms N] "
            "[--video-device N] [--pool PATH] [--rpmsg PATH] "
            "[--no-verify-crc]\n",
            name);
}

int main(int argc, char **argv)
{
    const char *pool_path = "/dev/k230-amp-camera-pool";
    const char *rpmsg_path = "/dev/rpmsg0";
    struct k230_amp_camera_pool_info pool_info = { 0 };
    struct capture_buffer buffers[K230_AMP_CAMERA_BUFFER_COUNT];
    struct v4l2_requestbuffers request = { 0 };
    struct v4l2_format format = { 0 };
    struct timespec started, deadline, now;
    uint64_t generation = 0;
    uint64_t sequence = 1;
    unsigned seconds = 10;
    unsigned timeout_ms = 5000;
    unsigned video_device = 2;
    unsigned exported = 0;
    unsigned captured = 0;
    unsigned submitted = 0;
    unsigned completed = 0;
    unsigned superseded = 0;
    unsigned verified = 0;
    unsigned crc_mismatch = 0;
    unsigned seen_mask = 0;
    unsigned i;
    uint32_t width, height, stride, data_length;
    uint64_t total_invalidate_cycles = 0;
    uint64_t total_crc_cycles = 0;
    double max_remote_ms = 0.0;
    double total_linux_crc_ms = 0.0;
    double max_linux_crc_ms = 0.0;
    int verify_crc = 1;
    int pool_fd = -1;
    int rpmsg_fd = -1;
    int video_fd = -1;
    int streaming = 0;
    int remote = -1;
    int pending = -1;
    int stopping = 0;
    int failed = 0;
    char video_path[64];

    memset(buffers, 0, sizeof(buffers));
    for (i = 0; i < K230_AMP_CAMERA_BUFFER_COUNT; ++i) {
        buffers[i].fd = -1;
        buffers[i].mapping = MAP_FAILED;
    }
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--pool") || !strcmp(argv[i], "--rpmsg")) {
            const char **target = !strcmp(argv[i], "--pool") ?
                                  &pool_path : &rpmsg_path;
            if (++i >= (unsigned)argc) {
                usage(argv[0]);
                return 2;
            }
            *target = argv[i];
        } else if (!strcmp(argv[i], "--seconds") ||
                   !strcmp(argv[i], "--timeout-ms") ||
                   !strcmp(argv[i], "--video-device")) {
            unsigned *target = !strcmp(argv[i], "--seconds") ? &seconds :
                               !strcmp(argv[i], "--timeout-ms") ?
                               &timeout_ms : &video_device;
            if (++i >= (unsigned)argc || parse_unsigned(argv[i], target) ||
                (target != &video_device && !*target)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--no-verify-crc")) {
            verify_crc = 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    rpmsg_fd = open(rpmsg_path, O_RDWR | O_CLOEXEC);
    if (rpmsg_fd < 0) {
        fprintf(stderr, "%s: %s\n", rpmsg_path, strerror(errno));
        return 1;
    }
    if (handshake(rpmsg_fd, sequence++, &generation, timeout_ms)) {
        failed = 1;
        goto out;
    }
    pool_fd = open(pool_path, O_RDONLY | O_CLOEXEC);
    if (pool_fd < 0 ||
        ioctl(pool_fd, K230_AMP_CAMERA_IOC_GET_INFO, &pool_info) < 0) {
        fprintf(stderr, "%s: %s\n", pool_path, strerror(errno));
        failed = 1;
        goto out;
    }
    if (pool_info.version != K230_AMP_CAMERA_POOL_ABI_VERSION ||
        pool_info.buffer_count != K230_AMP_CAMERA_BUFFER_COUNT ||
        pool_info.physical_base != K230_CAMERA_POOL_BASE ||
        pool_info.buffer_size != K230_CAMERA_BUFFER_SIZE) {
        fprintf(stderr, "camera pool does not match firmware ABI\n");
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
    width = format.fmt.pix.width;
    height = format.fmt.pix.height;
    stride = format.fmt.pix.bytesperline ?
             format.fmt.pix.bytesperline : width;
    if ((uint64_t)stride * height > UINT32_MAX) {
        fprintf(stderr, "Y plane geometry overflow\n");
        failed = 1;
        goto out;
    }
    data_length = stride * height;
    if (format.fmt.pix.sizeimage > pool_info.buffer_size ||
        padded_length(data_length) > pool_info.buffer_size) {
        fprintf(stderr, "camera image does not fit fixed buffer\n");
        failed = 1;
        goto out;
    }
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = K230_AMP_CAMERA_BUFFER_COUNT;
    if (ioctl(video_fd, VIDIOC_REQBUFS, &request) < 0 ||
        request.count < K230_AMP_CAMERA_BUFFER_COUNT) {
        fprintf(stderr, "VIDIOC_REQBUFS(DMABUF) failed: %s\n",
                strerror(errno));
        failed = 1;
        goto out;
    }

    for (i = 0; i < K230_AMP_CAMERA_BUFFER_COUNT; ++i) {
        struct k230_amp_camera_pool_buffer get = { .id = i };

        if (ioctl(pool_fd, K230_AMP_CAMERA_IOC_GET_BUFFER, &get) < 0) {
            fprintf(stderr, "GET_BUFFER[%u] failed: %s\n", i,
                    strerror(errno));
            failed = 1;
            goto out;
        }
        buffers[i].fd = get.fd;
        buffers[i].physical = get.physical;
        buffers[i].capacity = get.capacity;
        buffers[i].state = BUFFER_EXPORTED;
        ++exported;
        if (verify_crc) {
            buffers[i].mapping = mmap(NULL, get.capacity, PROT_READ,
                                      MAP_SHARED, get.fd, 0);
            if (buffers[i].mapping == MAP_FAILED) {
                fprintf(stderr, "mmap buffer[%u] failed: %s\n", i,
                        strerror(errno));
                failed = 1;
                goto out;
            }
        }
        if (register_buffer(rpmsg_fd, generation, sequence++, i,
                            &buffers[i], timeout_ms)) {
            failed = 1;
            goto out;
        }
        if (queue_buffer(video_fd, i, &buffers[i]) < 0) {
            fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n", i,
                    strerror(errno));
            failed = 1;
            goto out;
        }
    }

    printf("K230 RPMsg zero-copy camera integrity test\n");
    printf("generation=%" PRIu64 " pool=0x%" PRIx64
           " buffers=%u x %" PRIu64 "\n",
           generation, pool_info.physical_base, pool_info.buffer_count,
           pool_info.buffer_size);
    printf("camera=%s NV12 %ux%u stride=%u sizeimage=%u Y-bytes=%u "
           "duration=%us diagnostic-crc=%s\n",
           video_path, width, height, stride, format.fmt.pix.sizeimage,
           data_length, seconds, verify_crc ? "on" : "off");

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
    deadline = started;
    deadline.tv_sec += seconds;

    while (!failed && (!stopping || remote >= 0)) {
        struct pollfd fds[2] = {
            { .fd = rpmsg_fd, .events = POLLIN },
            { .fd = video_fd, .events = stopping ? 0 : POLLIN | POLLPRI },
        };
        int ready;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!stopping && (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec))) {
            stopping = 1;
            if (pending >= 0) {
                if (queue_buffer(video_fd, pending, &buffers[pending]) < 0)
                    failed = 1;
                pending = -1;
            }
            if (remote < 0)
                break;
            fds[1].events = 0;
        }
        do {
            ready = poll(fds, 2, stopping ? (int)timeout_ms : 1000);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (ready == 0 && stopping)) {
            fprintf(stderr, "event poll %s\n",
                    ready < 0 ? strerror(errno) : "timed out draining remote");
            failed = 1;
            break;
        }
        if (ready == 0)
            continue;

        if (fds[0].revents & POLLIN) {
            struct k230_rpmsg_camera_complete completion;
            int finished_remote = remote;
            uint32_t linux_crc = 0;
            int crc_result = 0;
            double remote_ms;

            if (read_completion(rpmsg_fd, generation, remote, buffers,
                                &completion)) {
                failed = 1;
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            remote_ms = elapsed_ms(&buffers[finished_remote].submitted_at,
                                   &now);
            if (remote_ms > max_remote_ms)
                max_remote_ms = remote_ms;
            total_invalidate_cycles += completion.invalidate_cycles;
            total_crc_cycles += completion.crc_cycles;
            if (verify_crc) {
                struct timespec crc_started, crc_finished;
                double linux_crc_ms;

                clock_gettime(CLOCK_MONOTONIC, &crc_started);
                crc_result = diagnostic_crc(&buffers[finished_remote],
                                            data_length,
                                            completion.observed_crc,
                                            &linux_crc);
                clock_gettime(CLOCK_MONOTONIC, &crc_finished);
                linux_crc_ms = elapsed_ms(&crc_started, &crc_finished);
                total_linux_crc_ms += linux_crc_ms;
                if (linux_crc_ms > max_linux_crc_ms)
                    max_linux_crc_ms = linux_crc_ms;
                ++verified;
                if (crc_result > 0) {
                    ++crc_mismatch;
                    fprintf(stderr,
                            "CRC mismatch buffer=%d sequence=%" PRIu64
                            " remote=%08x linux=%08x\n",
                            finished_remote,
                            buffers[finished_remote].sequence,
                            completion.observed_crc, linux_crc);
                } else if (crc_result < 0) {
                    failed = 1;
                }
            }
            ++completed;
            remote = -1;
            if (queue_buffer(video_fd, finished_remote,
                             &buffers[finished_remote]) < 0) {
                fprintf(stderr, "requeue completed buffer[%d] failed: %s\n",
                        finished_remote, strerror(errno));
                failed = 1;
                break;
            }
            if (!stopping && pending >= 0) {
                int next = pending;

                pending = -1;
                if (submit_buffer(rpmsg_fd, generation, sequence++, next,
                                  &buffers[next], data_length,
                                  width, height, stride)) {
                    failed = 1;
                    break;
                }
                remote = next;
                ++submitted;
            }
        }

        if (!failed && !stopping && (fds[1].revents & (POLLIN | POLLPRI))) {
            struct v4l2_buffer buffer = { 0 };
            int index;

            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_DMABUF;
            if (ioctl(video_fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN)
                    continue;
                fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
                failed = 1;
                break;
            }
            if (buffer.index >= K230_AMP_CAMERA_BUFFER_COUNT ||
                buffers[buffer.index].state != BUFFER_VI_QUEUED) {
                fprintf(stderr, "invalid dequeued buffer index/state %u/%u\n",
                        buffer.index, buffers[buffer.index].state);
                failed = 1;
                break;
            }
            index = (int)buffer.index;
            ++captured;
            seen_mask |= 1U << index;
            if (remote < 0) {
                if (submit_buffer(rpmsg_fd, generation, sequence++, index,
                                  &buffers[index], data_length,
                                  width, height, stride)) {
                    failed = 1;
                    break;
                }
                remote = index;
                ++submitted;
            } else {
                if (pending >= 0) {
                    if (queue_buffer(video_fd, pending,
                                     &buffers[pending]) < 0) {
                        failed = 1;
                        break;
                    }
                    ++superseded;
                }
                pending = index;
                buffers[index].state = BUFFER_PENDING;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (crc_mismatch)
        failed = 1;
    printf("capture: frames=%u submitted=%u completed=%u superseded=%u "
           "seen-mask=0x%x elapsed=%.3f s copies=0\n",
           captured, submitted, completed, superseded, seen_mask,
           elapsed_ms(&started, &now) / 1000.0);
    printf("integrity: verified=%u crc-mismatch=%u max-remote-hold=%.3f ms "
           "big-ms{invalidate-avg=%.3f,crc-avg=%.3f} "
           "linux-diagnostic-crc-ms{avg=%.3f,max=%.3f}\n",
           verified, crc_mismatch, max_remote_ms,
           completed ? total_invalidate_cycles * 1000.0 /
                       (BIG_CORE_HZ * completed) : 0.0,
           completed ? total_crc_cycles * 1000.0 /
                       (BIG_CORE_HZ * completed) : 0.0,
           verified ? total_linux_crc_ms / verified : 0.0,
           max_linux_crc_ms);

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
    for (i = 0; i < exported; ++i) {
        if (buffers[i].mapping != MAP_FAILED)
            munmap(buffers[i].mapping, buffers[i].capacity);
        close(buffers[i].fd);
    }
    if (pool_fd >= 0)
        close(pool_fd);
    if (rpmsg_fd >= 0)
        close(rpmsg_fd);
    printf("%s zero-copy camera-to-big-core CRC test\n",
           failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
