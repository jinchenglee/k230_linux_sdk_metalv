/*
 * Simple Camera Capture - Minimal ISP event handler + V4L2 capture
 * 
 * This bypasses the broken isp_media_server by:
 * 1. Subscribing to ISP events and acknowledging them
 * 2. Capturing frames directly via V4L2
 * 
 * Compile: riscv64-linux-gnu-gcc -o simple_camera_capture simple_camera_capture.c
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
#include <pthread.h>
#include <signal.h>

#define VVCAM_ISP_DEAMON_EVENT 0x08001000
#define ISP_SUBDEV_PATH "/dev/v4l-subdev0"
#define VIDEO_DEVICE "/dev/video1"
#define CAPTURE_WIDTH 1920
#define CAPTURE_HEIGHT 1080
#define BUFFER_COUNT 4

static volatile int running = 1;

void signal_handler(int sig) {
    printf("\nReceived signal %d, stopping...\n", sig);
    running = 0;
}

/* ISP Event Handler Thread */
void* isp_event_thread(void* arg) {
    int subdev_fd = *(int*)arg;
    struct v4l2_event_subscription sub;
    struct v4l2_event event;
    fd_set fds;
    struct timeval tv;
    int ret;

    printf("[ISP Event Thread] Starting...\n");

    /* Subscribe to all ISP events (ID 0-255) */
    for (int id = 0; id < 256; id++) {
        memset(&sub, 0, sizeof(sub));
        sub.type = VVCAM_ISP_DEAMON_EVENT;
        sub.id = id;
        sub.flags = V4L2_EVENT_SUB_FL_ALLOW_FEEDBACK;
        
        if (ioctl(subdev_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0) {
            printf("[ISP Event Thread] Subscribed to event ID %d\n", id);
        }
    }

    /* Event loop - acknowledge all events immediately */
    while (running) {
        FD_ZERO(&fds);
        FD_SET(subdev_fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ret = select(subdev_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select error");
            break;
        }
        if (ret == 0) continue;  // timeout

        /* Dequeue event */
        memset(&event, 0, sizeof(event));
        if (ioctl(subdev_fd, VIDIOC_DQEVENT, &event) < 0) {
            perror("VIDIOC_DQEVENT");
            continue;
        }

        printf("[ISP Event] Received event type=0x%08x id=%d\n", 
               event.type, event.id);

        /* 
         * Acknowledge the event immediately
         * The kernel expects the ack flag to be set in the event data
         * Based on vvcam_isp_event.c, the ack is in event_pkg->ack
         */
        struct {
            uint32_t eid;
            uint32_t ack;
            uint32_t reserved[14];
        } *event_data = (void*)event.u.data;
        
        event_data->ack = 1;
        
        /* Send back the acknowledgment via ioctl */
        struct v4l2_event ack_event = event;
        if (ioctl(subdev_fd, VIDIOC_DQEVENT, &ack_event) == 0) {
            printf("[ISP Event] Acknowledged event ID %d\n", event.id);
        }
    }

    printf("[ISP Event Thread] Stopping...\n");
    return NULL;
}

int main(int argc, char **argv) {
    int subdev_fd, video_fd;
    pthread_t event_tid;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    void *buffers[BUFFER_COUNT];
    int buffer_sizes[BUFFER_COUNT];
    int frame_count = 0;
    char filename[128];
    FILE *fp;

    printf("=== Simple Camera Capture ===\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open ISP subdevice */
    subdev_fd = open(ISP_SUBDEV_PATH, O_RDWR | O_NONBLOCK);
    if (subdev_fd < 0) {
        perror("Failed to open ISP subdevice");
        return 1;
    }
    printf("✓ Opened %s\n", ISP_SUBDEV_PATH);

    /* Start ISP event handler thread */
    if (pthread_create(&event_tid, NULL, isp_event_thread, &subdev_fd) != 0) {
        perror("Failed to create event thread");
        close(subdev_fd);
        return 1;
    }
    printf("✓ Started ISP event handler thread\n");
    
    sleep(1);  // Give event handler time to subscribe

    /* Open video device */
    video_fd = open(VIDEO_DEVICE, O_RDWR | O_NONBLOCK);
    if (video_fd < 0) {
        perror("Failed to open video device");
        running = 0;
        pthread_join(event_tid, NULL);
        close(subdev_fd);
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

    /* Get current format (use device default: 1920x1080 NV16) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(video_fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
        goto cleanup;
    }
    printf("✓ Current format: %dx%d fourcc=0x%08x\n", 
           fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

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
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int ret = select(video_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) {
            printf("Timeout waiting for frame\n");
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
            snprintf(filename, sizeof(filename), "/tmp/frame_%dx%d.yuv",
                    fmt.fmt.pix.width, fmt.fmt.pix.height);
            fp = fopen(filename, "wb");
            if (fp) {
                fwrite(buffers[buf.index], buf.bytesused, 1, fp);
                fclose(fp);
                printf("✓ Saved frame to %s (fourcc=0x%08x)\n", filename, fmt.fmt.pix.pixelformat);
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
    
    running = 0;
    pthread_join(event_tid, NULL);
    close(subdev_fd);
    printf("✓ Closed ISP subdevice\n");

    printf("\nCaptured %d frames successfully!\n", frame_count);
    return 0;
}

