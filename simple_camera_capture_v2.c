/*
 * Simple Camera Capture V2 - Works with isp_media_server
 * 
 * This version relies on isp_media_server to handle ISP events.
 * We just capture frames directly from /dev/video1
 * 
 * Prerequisites:
 *   1. isp_media_server must be running in background
 *   2. Sensor configured in /proc/vsi/isp_subdev0
 * 
 * Compile: riscv64-unknown-linux-gnu-gcc -o simple_camera_capture_v2 simple_camera_capture_v2.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <signal.h>

#define VIDEO_DEVICE "/dev/video1"
#define BUFFER_COUNT 4

static volatile int running = 1;

void signal_handler(int sig) {
    printf("\nReceived signal %d, stopping...\n", sig);
    running = 0;
}

int main(int argc, char **argv) {
    int video_fd;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    void *buffers[BUFFER_COUNT];
    int buffer_sizes[BUFFER_COUNT];
    int frame_count = 0;
    char filename[128];
    FILE *fp;

    printf("=== Simple Camera Capture V2 ===\n");
    printf("Make sure isp_media_server is running!\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Wait a moment for isp_media_server to initialize */
    sleep(1);

    /* Open video device */
    video_fd = open(VIDEO_DEVICE, O_RDWR);
    if (video_fd < 0) {
        perror("Failed to open video device");
        return 1;
    }
    printf("✓ Opened %s\n", VIDEO_DEVICE);

    /* Query capabilities */
    if (ioctl(video_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto cleanup;
    }
    printf("✓ Driver: %s\n", cap.driver);
    printf("✓ Card: %s\n", cap.card);

    /* Get current format (use whatever isp_media_server configured) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(video_fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
        goto cleanup;
    }
    
    char fourcc[5] = {0};
    memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
    printf("✓ Current format: %dx%d %s (fourcc=0x%08x)\n", 
           fmt.fmt.pix.width, fmt.fmt.pix.height, fourcc, fmt.fmt.pix.pixelformat);

    /* Request buffers */
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto cleanup;
    }
    printf("✓ Requested %d buffers\n", req.count);

    /* Map buffers */
    for (int i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto cleanup;
        }

        buffer_sizes[i] = buf.length;
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, video_fd, buf.m.offset);
        
        if (buffers[i] == MAP_FAILED) {
            perror("mmap");
            goto cleanup;
        }

        /* Queue buffer */
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }
    }
    printf("✓ Mapped and queued %d buffers\n", req.count);

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto cleanup;
    }
    printf("✓ Streaming started\n");
    printf("\nCapturing frames (Ctrl+C to stop)...\n");

    /* Capture loop */
    while (running && frame_count < 10) {
        fd_set fds;
        struct timeval tv;

        FD_ZERO(&fds);
        FD_SET(video_fd, &fds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ret = select(video_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) {
            printf("Timeout waiting for frame (isp_media_server running?)\n");
            continue;
        }

        /* Dequeue buffer */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(video_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF");
            break;
        }

        frame_count++;
        printf("Frame %d: %d bytes, sequence %d\n", 
               frame_count, buf.bytesused, buf.sequence);

        /* Save first frame */
        if (frame_count == 1) {
            snprintf(filename, sizeof(filename), "/tmp/frame_%dx%d_%s.yuv",
                    fmt.fmt.pix.width, fmt.fmt.pix.height, fourcc);
            fp = fopen(filename, "wb");
            if (fp) {
                fwrite(buffers[buf.index], buf.bytesused, 1, fp);
                fclose(fp);
                printf("✓ Saved frame to %s\n", filename);
            }
        }

        /* Requeue buffer */
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    /* Stop streaming */
    if (ioctl(video_fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
    }
    printf("✓ Streaming stopped\n");

cleanup:
    /* Cleanup */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i] != MAP_FAILED && buffers[i] != NULL) {
            munmap(buffers[i], buffer_sizes[i]);
        }
    }
    
    close(video_fd);
    printf("✓ Closed video device\n");

    printf("\nCaptured %d frames successfully!\n", frame_count);
    return 0;
}

