#pragma once
#ifndef __V4L2_DRM_H__
#define __V4L2_DRM_H__

#define DRM_BUFFERING 2

#include "display.h"
#include <stdint.h>
#include <stdbool.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

struct v4l2_drm_video_buffer {
    void* mmap;
    int fd;
    unsigned index;
};

struct v4l2_crop_size {
    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t crop_en;
};

struct v4l2_drm_context {
    unsigned width;
    unsigned height;
    /* DRM destination rectangle. Zero preserves framebuffer dimensions. */
    unsigned display_width;
    unsigned display_height;
    /* Maximum event-driven atomic submissions per second; zero is unlimited. */
    unsigned max_display_fps;
    unsigned device;
    int video_fd;
    unsigned frame_count; //sensor 帧数
    uint32_t video_format;
    uint32_t display_format;
    bool display;
    unsigned buffer_num; //显示或者v4l2 需要的buffer数量；
    struct display_plane* plane; //本v4l2对应 plane ；
    struct display_buffer** display_buffers; //v4l2对应 plane里面的display_buffers
    struct v4l2_drm_video_buffer* buffers;   //v4l2 buffers
    struct v4l2_crop_size crop_size;
    unsigned offset_x;
    unsigned offset_y;
    bool flag_dqbuf;  // 是否已完成 DQBUF（出队）但尚未显示" 的标志
    uint8_t wp; //Write Position，是双缓冲环形队列的写指针
    struct v4l2_buffer vbuffer; //当前使用的v4l2_buffer
    int buffer_hold[DRM_BUFFERING];
    bool flag_dump; //dump 标记
    enum drm_rotation drm_rotation;
    int8_t hflip;
    int8_t vflip;
};


typedef int(*v4l2_drm_handler)(struct v4l2_drm_context* ctx, bool displayed);

void v4l2_drm_default_context(struct v4l2_drm_context* ctx);
/*
 * Request a physical-camera mode by capability.  The active sensor driver
 * resolves its own opaque mode index.  Returns 0 for the preferred mode, 1
 * for the fallback mode, and -1 when neither is supported or configuration
 * cannot be applied.  Must be called before capture starts.
 */
int v4l2_drm_request_sensor_mode(unsigned device,
                                  uint16_t preferred_width, uint16_t preferred_height,
                                  uint32_t preferred_fps,
                                  uint16_t fallback_width, uint16_t fallback_height,
                                  uint32_t fallback_fps,
                                  uint16_t *selected_width, uint16_t *selected_height,
                                  uint32_t *selected_fps);
/* Opt-in luma-only capture: programs the ISP color (CPROC) block to output
 * neutral chroma (grayscale), so the NV12 stream both detection and the
 * hardware display plane read carries Y/luma only. This is a one-time
 * control-plane call at setup -- zero per-frame CPU cost -- and is additive:
 * applications that never call it keep the previous color path unchanged.
 * Returns 0 on success (device found and control accepted), -1 otherwise. */
int v4l2_drm_set_luma_only(unsigned device, bool enable);
// use /dev/dri/card0 as default
int v4l2_drm_setup(struct v4l2_drm_context context[], unsigned num, struct display** display);
/**
 *
 * @param fps Array of FPS output, NULL if not used
 */
int v4l2_drm_run(struct v4l2_drm_context ctx[], unsigned num, v4l2_drm_handler handler);
/*
 * Opt-in update-driven scheduler: submit only fresh camera/OSD/LVGL buffers
 * while preserving the legacy v4l2_drm_run callback cadence for other apps.
 */
int v4l2_drm_run_event_driven(struct v4l2_drm_context ctx[], unsigned num,
                              v4l2_drm_handler handler);
int v4l2_drm_run_v4l2_2_drm(struct v4l2_drm_context ctx[], unsigned num, v4l2_drm_handler handler);


int v4l2_drm_start(const struct v4l2_drm_context* context);
int v4l2_drm_stop(const struct v4l2_drm_context* context);
int v4l2_drm_dump(struct v4l2_drm_context* context, int timeout);
/* Latest-wins variant of v4l2_drm_dump(): waits for readiness, then drains the
 * driver's done-queue FIFO, immediately requeuing (QBUF) every older buffer and
 * keeping only the newest one in context->vbuffer. This discards queued stale
 * frames; as with the legacy API, a buffer remains held until dump_release(). */
int v4l2_drm_dump_latest(struct v4l2_drm_context* context, int timeout);
int v4l2_drm_dump_release(struct v4l2_drm_context* context);
extern bool v4l2_drm_run_v4l2_2_drm_need_run;
#ifdef __cplusplus
}
#endif

#endif
