#include "common.h"
#include "display.h"
#include "v4l2-drm.h"
#include <display.h>
#include <linux/videodev2.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <unistd.h>
#include <dlfcn.h>
#include <v4l2-drm.h>

/* Kept in sync with vvcam_isp_driver.c and v4l2-drm-scene. */
#define V4L2_DRM_SCENE_CID_BASE V4L2_CID_USER_BASE

struct v4l2_drm_scene_config {
    char scene_path[128];
    char sensor[32];
    char xml_file[128];
    char manu_json_file[128];
    char auto_json_file[128];
    uint32_t mode;
};

static int v4l2_drm_get_scene_config(int fd, struct v4l2_drm_scene_config *cfg)
{
    struct v4l2_ext_controls ctrls = {0};
    struct v4l2_ext_control ctrl[6] = {0};

    ctrl[0].id = V4L2_DRM_SCENE_CID_BASE;
    ctrl[0].size = sizeof(cfg->scene_path);
    ctrl[0].string = cfg->scene_path;
    ctrl[1].id = V4L2_DRM_SCENE_CID_BASE + 1;
    ctrl[1].size = sizeof(cfg->sensor);
    ctrl[1].string = cfg->sensor;
    ctrl[2].id = V4L2_DRM_SCENE_CID_BASE + 2;
    ctrl[2].size = sizeof(cfg->xml_file);
    ctrl[2].string = cfg->xml_file;
    ctrl[3].id = V4L2_DRM_SCENE_CID_BASE + 3;
    ctrl[3].size = sizeof(cfg->manu_json_file);
    ctrl[3].string = cfg->manu_json_file;
    ctrl[4].id = V4L2_DRM_SCENE_CID_BASE + 4;
    ctrl[4].size = sizeof(cfg->auto_json_file);
    ctrl[4].string = cfg->auto_json_file;
    ctrl[5].id = V4L2_DRM_SCENE_CID_BASE + 5;
    ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ctrls.count = 6;
    ctrls.controls = ctrl;
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) < 0)
        return -1;
    cfg->mode = ctrl[5].value;
    return 0;
}

static int v4l2_drm_set_scene_mode(int fd, const struct v4l2_drm_scene_config *cfg,
                                   uint32_t mode)
{
    struct v4l2_ext_controls ctrls = {0};
    struct v4l2_ext_control ctrl[6] = {0};

    ctrl[0].id = V4L2_DRM_SCENE_CID_BASE;
    ctrl[0].size = strlen(cfg->scene_path) + 1;
    ctrl[0].string = (char *)cfg->scene_path;
    ctrl[1].id = V4L2_DRM_SCENE_CID_BASE + 1;
    ctrl[1].size = strlen(cfg->sensor) + 1;
    ctrl[1].string = (char *)cfg->sensor;
    ctrl[2].id = V4L2_DRM_SCENE_CID_BASE + 2;
    ctrl[2].size = strlen(cfg->xml_file) + 1;
    ctrl[2].string = (char *)cfg->xml_file;
    ctrl[3].id = V4L2_DRM_SCENE_CID_BASE + 3;
    ctrl[3].size = strlen(cfg->manu_json_file) + 1;
    ctrl[3].string = (char *)cfg->manu_json_file;
    ctrl[4].id = V4L2_DRM_SCENE_CID_BASE + 4;
    ctrl[4].size = strlen(cfg->auto_json_file) + 1;
    ctrl[4].string = (char *)cfg->auto_json_file;
    ctrl[5].id = V4L2_DRM_SCENE_CID_BASE + 5;
    ctrl[5].value = mode;
    ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ctrls.count = 6;
    ctrls.controls = ctrl;
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
}

/* A sensor mode is usable only when the active ISP scene has a matching
 * calibration resolution.  The closed isp_media_server reports this only at
 * STREAMON, so reject it here and allow the caller's fallback instead. */
static int v4l2_drm_scene_supports_resolution(const struct v4l2_drm_scene_config *cfg,
                                              uint16_t width, uint16_t height)
{
    char path[sizeof(cfg->scene_path) + sizeof(cfg->xml_file) + 2];
    char resolution[32];
    char line[256];
    FILE *xml;

    if (snprintf(path, sizeof(path), "%s/%s", cfg->scene_path, cfg->xml_file) >=
        (int)sizeof(path))
        return 0;
    if (snprintf(resolution, sizeof(resolution), "%ux%u", width, height) >=
        (int)sizeof(resolution))
        return 0;
    xml = fopen(path, "r");
    if (!xml)
        return 0;
    while (fgets(line, sizeof(line), xml)) {
        if (strstr(line, resolution)) {
            fclose(xml);
            return 1;
        }
    }
    fclose(xml);
    return 0;
}

static int v4l2_drm_mode_filename(char *out, size_t out_size,
                                  const char *filename,
                                  uint16_t width, uint16_t height)
{
    /* Keep multi-part suffixes (for example .manual.json) intact so all
     * siblings share the XML profile's <stem>-<resolution> naming. */
    const char *suffix = strchr(filename, '.');
    size_t base_len = suffix ? (size_t)(suffix - filename) : strlen(filename);
    int written = snprintf(out, out_size, "%.*s-%ux%u%s", (int)base_len,
                           filename, width, height, suffix ? suffix : "");

    return written >= 0 && (size_t)written < out_size;
}

/* Look for a sibling per-mode scene.  This derives profile names from the
 * active scene rather than from a sensor name: e.g. ov5647.xml becomes
 * ov5647-1280x720.xml.  A future camera enables a mode by shipping its own
 * three matching files under the same convention. */
static int v4l2_drm_select_scene_profile(struct v4l2_drm_scene_config *cfg,
                                         uint16_t width, uint16_t height)
{
    struct v4l2_drm_scene_config candidate = *cfg;
    char path[sizeof(cfg->scene_path) + sizeof(cfg->xml_file) + 2];

    if (v4l2_drm_scene_supports_resolution(cfg, width, height))
        return 1;
    if (!v4l2_drm_mode_filename(candidate.xml_file, sizeof(candidate.xml_file),
                                 cfg->xml_file, width, height) ||
        !v4l2_drm_mode_filename(candidate.manu_json_file,
                                 sizeof(candidate.manu_json_file),
                                 cfg->manu_json_file, width, height) ||
        !v4l2_drm_mode_filename(candidate.auto_json_file,
                                 sizeof(candidate.auto_json_file),
                                 cfg->auto_json_file, width, height))
        return 0;
    if (snprintf(path, sizeof(path), "%s/%s", candidate.scene_path,
                 candidate.xml_file) >= (int)sizeof(path) || access(path, R_OK) != 0)
        return 0;
    if (snprintf(path, sizeof(path), "%s/%s", candidate.scene_path,
                 candidate.manu_json_file) >= (int)sizeof(path) || access(path, R_OK) != 0)
        return 0;
    if (snprintf(path, sizeof(path), "%s/%s", candidate.scene_path,
                 candidate.auto_json_file) >= (int)sizeof(path) || access(path, R_OK) != 0)
        return 0;
    if (!v4l2_drm_scene_supports_resolution(&candidate, width, height))
        return 0;

    fprintf(stderr, "v4l2-drm: using per-mode scene %s (starter tuning)\n",
            candidate.xml_file);
    *cfg = candidate;
    return 1;
}

int v4l2_drm_request_sensor_mode(unsigned device,
                                  uint16_t preferred_width, uint16_t preferred_height,
                                  uint32_t preferred_fps,
                                  uint16_t fallback_width, uint16_t fallback_height,
                                  uint32_t fallback_fps,
                                  uint16_t *selected_width, uint16_t *selected_height,
                                  uint32_t *selected_fps)
{
    typedef int (*find_mode_fn)(const char *, uint16_t, uint16_t, uint32_t, uint32_t *);
    char path[64];
    struct v4l2_drm_scene_config cfg = {0};
    find_mode_fn find_mode;
    void *sensor_lib;
    uint32_t mode;
    int fd;
    int result;

    snprintf(path, sizeof(path), "/dev/video%u", device);
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "v4l2-drm: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (v4l2_drm_get_scene_config(fd, &cfg) < 0) {
        fprintf(stderr, "v4l2-drm: cannot read active camera scene: %s\n",
                strerror(errno));
        goto fail_close;
    }

    sensor_lib = dlopen("libvvcam-capabilities.so", RTLD_NOW | RTLD_LOCAL);
    if (!sensor_lib) {
        fprintf(stderr, "v4l2-drm: cannot load sensor capability library: %s\n",
                dlerror());
        goto fail_close;
    }
    find_mode = (find_mode_fn)dlsym(sensor_lib, "vvcam_sensor_find_mode");
    if (!find_mode) {
        fprintf(stderr, "v4l2-drm: sensor capability lookup is unavailable: %s\n",
                dlerror());
        goto fail_dl;
    }

    result = find_mode(cfg.sensor, preferred_width, preferred_height,
                       preferred_fps, &mode);
    if (result == 0 && !v4l2_drm_select_scene_profile(
                           &cfg, preferred_width, preferred_height))
        result = -1;
    if (result == 0) {
        result = 0;
        if (selected_width) *selected_width = preferred_width;
        if (selected_height) *selected_height = preferred_height;
        if (selected_fps) *selected_fps = preferred_fps;
    } else if (find_mode(cfg.sensor, fallback_width, fallback_height,
                         fallback_fps, &mode) == 0 &&
               v4l2_drm_select_scene_profile(
                   &cfg, fallback_width, fallback_height)) {
        result = 1;
        if (selected_width) *selected_width = fallback_width;
        if (selected_height) *selected_height = fallback_height;
        if (selected_fps) *selected_fps = fallback_fps;
    } else {
        fprintf(stderr,
                "v4l2-drm: active sensor '%s' has neither %ux%u@%u nor %ux%u@%u\n",
                cfg.sensor, preferred_width, preferred_height, preferred_fps,
                fallback_width, fallback_height, fallback_fps);
        goto fail_dl;
    }
    if (v4l2_drm_set_scene_mode(fd, &cfg, mode) < 0) {
        fprintf(stderr, "v4l2-drm: cannot select camera mode %u: %s\n",
                mode, strerror(errno));
        goto fail_dl;
    }
    dlclose(sensor_lib);
    close(fd);
    return result;

fail_dl:
    dlclose(sensor_lib);
fail_close:
    close(fd);
    return -1;
}

/* CPROC (color processing) control ids, ISP-side.  These are driven through
 * the video device's VIDIOC_S_CTRL, which the kernel forwards to the ISP
 * subdev control handler (vvcam_isp_s_ctrl -> v4l2_s_ctrl) where the CPROC
 * block is registered.  Values match vvcam_isp_cproc.h. */
#define V4L2_DRM_CPROC_BASE            (V4L2_CID_USER_BASE + 0x2900)
#define V4L2_DRM_CPROC_ENABLE         (V4L2_DRM_CPROC_BASE + 0x0000)
#define V4L2_DRM_CPROC_MODE           (V4L2_DRM_CPROC_BASE + 0x0003) /* 0=auto 1=manual */
#define V4L2_DRM_CPROC_MANU_SATURATION (V4L2_DRM_CPROC_BASE + 0x000C)

int v4l2_drm_set_luma_only(unsigned device, bool enable)
{
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/dev/video%u", device);
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "v4l2-drm: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Enable the CPROC block and switch it to manual control so the
     * saturation value we set is honored (not overridden by auto tuning). */
    if (enable) {
        if (ioctl(fd, VIDIOC_S_CTRL, &(struct v4l2_control){.id = V4L2_DRM_CPROC_ENABLE, .value = 1}) < 0 ||
            ioctl(fd, VIDIOC_S_CTRL, &(struct v4l2_control){.id = V4L2_DRM_CPROC_MODE, .value = 1}) < 0 ||
            ioctl(fd, VIDIOC_S_CTRL, &(struct v4l2_control){.id = V4L2_DRM_CPROC_MANU_SATURATION, .value = 0}) < 0) {
            fprintf(stderr, "v4l2-drm: cannot set luma-only CPROC on %s: %s\n",
                    path, strerror(errno));
            close(fd);
            return -1;
        }
    } else {
        if (ioctl(fd, VIDIOC_S_CTRL, &(struct v4l2_control){.id = V4L2_DRM_CPROC_MODE, .value = 0}) < 0 ||
            ioctl(fd, VIDIOC_S_CTRL, &(struct v4l2_control){.id = V4L2_DRM_CPROC_ENABLE, .value = 0}) < 0) {
            fprintf(stderr, "v4l2-drm: cannot restore color CPROC on %s: %s\n",
                    path, strerror(errno));
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

void v4l2_drm_default_context(struct v4l2_drm_context* ctx) {
    memset(ctx, 0 , sizeof(*ctx));
    ctx->width = 640;
    ctx->height = 480;
    ctx->device = 0;
    ctx->video_fd = -1;
    ctx->video_format = V4L2_PIX_FMT_NV12;
    ctx->display_format = DRM_FORMAT_NV12;
    ctx->display = true;
    ctx->buffer_num = DRM_BUFFERING+2;
    ctx->plane = NULL;
    ctx->flag_dqbuf = true;
    ctx->offset_x = 0;
    ctx->offset_y = 0;
    ctx->frame_count = 0;
    ctx->flag_dqbuf = false;
    for (unsigned i = 0; i < DRM_BUFFERING; i++) {
        ctx->buffer_hold[i] = -1;
    }
    ctx->wp = 0;
    ctx->flag_dump = false;
    ctx->drm_rotation = rotation_0;
    ctx->hflip = -1;
    ctx->vflip = -1;
}

static int v4l2_drm_set_control(int fd, uint32_t id, int value)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    return ioctl(fd, VIDIOC_S_CTRL, &ctrl);
}

static uint32_t v4l2_to_drm(uint32_t fourcc) {
    switch (fourcc) {
        case V4L2_PIX_FMT_NV12: return DRM_FORMAT_NV12;
        case V4L2_PIX_FMT_NV21: return DRM_FORMAT_NV21;
        case V4L2_PIX_FMT_NV16: return DRM_FORMAT_NV16;
        case V4L2_PIX_FMT_NV61: return DRM_FORMAT_NV61;
        case V4L2_PIX_FMT_BGR24: return DRM_FORMAT_BGR888;
        case V4L2_PIX_FMT_RGB24: return DRM_FORMAT_RGB888;
        case V4L2_PIX_FMT_YUYV: return DRM_FORMAT_YUYV;
        default:
            pr(
                "no plane for video format %c%c%c%c",
                (fourcc >> 0) & 0xff,
                (fourcc >> 8) & 0xff,
                (fourcc >> 16) & 0xff,
                (fourcc >> 24) & 0xff
                );
            return 0;
    }
}

int v4l2_drm_setup(struct v4l2_drm_context context[], unsigned num, struct display** display) {
    struct display* d = NULL;
    if (display && *display) {
        d = *display;
    }
    for (unsigned i = 0; i < num; i++) {
        context[i].buffers = NULL;
        context[i].display_buffers = NULL;
        if (context[i].display && context[i].plane == NULL) {
            if (d == NULL) {
                d = display_init(0);
                CKE(d == NULL, close);
            }
            if (context[i].display_format == 0) {
                context[i].display_format = v4l2_to_drm(context[i].video_format);
                CKE(context[i].display_format == 0, close);
            }
            d->drm_rotation = context[i].drm_rotation;
            context[i].plane = display_get_plane(d, context[i].display_format);
            CKE(context[i].plane == NULL, close);
            for (unsigned j = 0; j < context[i].buffer_num; j++) {
                if(context[i].display_format == DRM_FORMAT_NV12)
                {
                    if((context[i].drm_rotation == rotation_90) || (context[i].drm_rotation == rotation_270))
                    {
                        context[i].plane->drm_rotation = context[i].drm_rotation;
                        CKE(display_allocate_buffer(context[i].plane, context[i].height, context[i].width) == NULL, close);
                    }
                    else
                        CKE(display_allocate_buffer(context[i].plane, context[i].width, context[i].height) == NULL, close);
                }
                else {
                    CKE(display_allocate_buffer(context[i].plane, context[i].width, context[i].height) == NULL, close);
                }
            }
        }
        char cam_device_path[64];
        snprintf(cam_device_path, sizeof(cam_device_path) - 1, "/dev/video%u", context[i].device);
        context[i].video_fd = open(cam_device_path, O_RDWR | O_NONBLOCK);
        CKE(context[i].video_fd < 0, close);

        struct v4l2_capability capbility;
        CKE(ioctl(context[i].video_fd, VIDIOC_QUERYCAP, &capbility), close);

        struct v4l2_fmtdesc fmtdesc;
        memset(&fmtdesc, 0, sizeof(fmtdesc));
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        while (ioctl(context[i].video_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
            pr(
                "/dev/video%u support format %c%c%c%c",
                context[i].device,
                (fmtdesc.pixelformat >> 0) & 0xff,
                (fmtdesc.pixelformat >> 8) & 0xff,
                (fmtdesc.pixelformat >> 16) & 0xff,
                (fmtdesc.pixelformat >> 24) & 0xff
            );
            fmtdesc.index += 1;
        }

        // struct v4l2_crop crop;
        // struct v4l2_cropcap cropcap;
        // if (-1 == ioctl (context[i].video_fd, VIDIOC_CROPCAP, &cropcap)) {
        //     perror ("VIDIOC_CROPCAP");
        //     // exit (EXIT_FAILURE);
        // }
        // printf("--------------------cropcap.widt is %d ------------dadadadad ---------------------- \n", cropcap.bounds.width);

        struct v4l2_format format;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        CKE(ioctl(context[i].video_fd, VIDIOC_G_FMT, &format), close);
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.pixelformat = context[i].video_format;
        format.fmt.pix.width = context[i].width;
        format.fmt.pix.height = context[i].height;
        CKE(ioctl(context[i].video_fd, VIDIOC_S_FMT, &format), close);

        if (context[i].hflip >= 0) {
            CKE(v4l2_drm_set_control(context[i].video_fd, V4L2_CID_HFLIP, context[i].hflip), close);
        }
        if (context[i].vflip >= 0) {
            CKE(v4l2_drm_set_control(context[i].video_fd, V4L2_CID_VFLIP, context[i].vflip), close);
        }

        if((context[i].crop_size.height != 0) && (context[i].crop_size.width != 0) &&
                (context[i].crop_size.height > context[i].height ) && (context[i].crop_size.width > context[i].width))
        {
            printf("set crop \n");
#if 1
            // set crop
            struct v4l2_selection sel = {
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                .target = V4L2_SEL_TGT_COMPOSE_BOUNDS,
            };
            struct v4l2_rect r;
            int ret = 0;

            ret = ioctl(context[i].video_fd, VIDIOC_G_SELECTION, &sel);
            if(ret < 0)
                printf("get VIDIOC_G_SELECTION err \n");

            /* setting smaller compose rectangle */
            r.width = context[i].crop_size.width;
            r.height = context[i].crop_size.height;
            r.left = context[i].crop_size.offset_x;
            r.top = context[i].crop_size.offset_y;

            sel.r = r;
            sel.target = V4L2_SEL_TGT_COMPOSE;
            sel.flags = V4L2_SEL_FLAG_LE;

            ret = ioctl(context[i].video_fd, VIDIOC_S_SELECTION, &sel);
            if(ret < 0)
                printf("get VIDIOC_S_SELECTION err \n");
#else
            struct v4l2_crop crop;
            struct v4l2_cropcap cropcap;

            memset (&cropcap, 0, sizeof (cropcap));
            cropcap.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

            if (-1 == ioctl (context[i].video_fd, VIDIOC_CROPCAP, &cropcap)) {
                perror ("VIDIOC_CROPCAP");
            }

            memset (&crop, 0, sizeof (crop));
            crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            crop.c = cropcap.defrect;

            crop.c.width = context[i].crop_size.width;
            crop.c.height = context[i].crop_size.height;
            crop.c.left = context[i].crop_size.offset_x;
            crop.c.top = context[i].crop_size.offset_y;

            /* Ignore if cropping is not supported (EINVAL). */

            if (-1 == ioctl (context[i].video_fd, VIDIOC_S_CROP, &crop)
                && errno != EINVAL) {
                perror ("VIDIOC_S_CROP");
            }
            printf("set crop crop.c.height is %d \n", crop.c.height);
#endif
        }

        struct v4l2_requestbuffers request_buffer;
        memset(&request_buffer, 0, sizeof(request_buffer));
        request_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (context[i].display) {
            request_buffer.memory = V4L2_MEMORY_DMABUF;
        } else {
            request_buffer.memory = V4L2_MEMORY_MMAP;
        }
        request_buffer.count = context[i].buffer_num;
        CKE(ioctl(context[i].video_fd, VIDIOC_REQBUFS, &request_buffer), close);
        context[i].buffers = malloc(sizeof(struct v4l2_drm_video_buffer) * context[i].buffer_num);
        CKE(context[i].buffers == NULL, close);
        memset(context[i].buffers, 0,
               sizeof(struct v4l2_drm_video_buffer) * context[i].buffer_num);
        for (unsigned bi = 0; bi < context[i].buffer_num; bi++) {
            context[i].buffers[bi].fd = -1;
        }
        if (context[i].display) {
            struct display_buffer* db = context[i].plane->buffers;
            context[i].display_buffers = malloc(sizeof(struct display_buffer) * context[i].buffer_num);
            for (unsigned j = 0; j < context[i].buffer_num; j++) {
                memset(&context[i].vbuffer, 0, sizeof(context[i].vbuffer));
                context[i].vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                context[i].vbuffer.memory = V4L2_MEMORY_DMABUF;
                context[i].vbuffer.index = j;
                context[i].vbuffer.m.fd = db->dmabuf_fd;
                #if DEBUG_SEQ
                pr("DEBUG: index %u dmabuf %d", j, db->dmabuf_fd);
                #endif
                context[i].vbuffer.length = db->size;
                CKE(ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer), close);
                context[i].display_buffers[j] = db;
                context[i].buffers[j].mmap = db->map;
                context[i].buffers[j].fd = db->dmabuf_fd;
                context[i].buffers[j].index = j;
                db = db->next;
            }
        } else {
            for (unsigned j = 0; j < context[i].buffer_num; j++) {
                memset(&context[i].vbuffer, 0, sizeof(context[i].vbuffer));
                context[i].vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                context[i].vbuffer.memory = V4L2_MEMORY_MMAP;
                context[i].vbuffer.index = j;
                CKE(ioctl(context[i].video_fd, VIDIOC_QUERYBUF, &context[i].vbuffer), close);
                CKE(ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer), close);
                context[i].buffers[j].mmap = mmap(
                    NULL,
                    context[i].vbuffer.length,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    context[i].video_fd,
                    context[i].vbuffer.m.offset
                );
                CKE(context[i].buffers[j].mmap == MAP_FAILED, close);
                // export DMA-buf
                struct v4l2_exportbuffer expbuf;
                memset(&expbuf, 0, sizeof(expbuf));
                expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                expbuf.index = j;
                CKE(ioctl(context[i].video_fd, VIDIOC_EXPBUF, &expbuf), close);
                context[i].buffers[j].fd = expbuf.fd;
                context[i].buffers[j].index = j;
            }
        }
        continue;

        close:
        for (unsigned j = 0; j <= i; j++) {
            close(context[i].video_fd);
            context[i].video_fd = -1;
            if (context[i].buffers) {
                free(context[i].buffers);
            }
            if (context[i].display_buffers) {
                free(context[i].display_buffers);
            }
        }
        if (display && (*display == NULL)) {
            display_exit(d);
        }
        return -1;
    }
    if (display) {
        *display = d;
    }
    return 0;
}

static unsigned dump_count = 0;

static bool v4l2_drm_have_pending_display_update(
    const struct display* d,
    const struct v4l2_drm_context context[],
    unsigned num)
{
    if (d->lvgl_disp_buffer || d->osd_disp_buffer)
        return true;

    for (unsigned i = 0; i < num; i++) {
        if (context[i].display && context[i].flag_dqbuf &&
            context[i].buffer_hold[context[i].wp] >= 0)
            return true;
    }
    return false;
}

static int v4l2_drm_commit_pending_display_update(
    struct display* d,
    struct v4l2_drm_context context[],
    unsigned num)
{
    for (unsigned i = 0; i < num; i++) {
        if (!context[i].display || !context[i].flag_dqbuf ||
            context[i].buffer_hold[context[i].wp] < 0)
            continue;

        if (display_update_buffer(
                context[i].display_buffers[context[i].buffer_hold[context[i].wp]],
                context[i].offset_x, context[i].offset_y))
            return -1;
        context[i].flag_dqbuf = false;
    }

    if (d->osd_disp_buffer) {
        if (display_update_buffer(d->osd_disp_buffer, 0, 0))
            return -1;
        d->osd_disp_buffer = NULL;
    }
    if (d->lvgl_disp_buffer) {
        if (display_update_buffer(d->lvgl_disp_buffer, 0, 0))
            return -1;
        d->lvgl_disp_buffer = NULL;
    }

    if (display_commit(d))
        return -1;
    d->frame_count += 1;
    return 0;
}

static void dump_file(const struct v4l2_drm_context* ctx, unsigned channel) {
    char filename[128];
    snprintf(
        filename,
        sizeof(filename),
        "Image_%u_%u_%ux%u.%c%c%c%c",
        channel, dump_count,
        ctx->width, ctx->height,
        (ctx->video_format >> 0) & 0xff,
        (ctx->video_format >> 8) & 0xff,
        (ctx->video_format >> 16) & 0xff,
        (ctx->video_format >> 24) & 0xff
    );
    FILE* f = fopen(filename, "w");
    if (f == NULL) {
        pr("open %s error %d(%s)", filename, errno, strerror(errno));
        return;
    }
    fwrite(ctx->buffers[ctx->vbuffer.index].mmap, 1, ctx->vbuffer.length, f);
    fclose(f);
    pr("dump file to %s", filename);
}

static int v4l2_drm_run_impl(struct v4l2_drm_context context[], unsigned num,
                             v4l2_drm_handler handler, bool event_driven) {
    int flag_enable_display = 0;
    int display_fd;
    struct display* d = NULL;
    for (unsigned i = 0; i < num; i++) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        CKE(ioctl(context[i].video_fd, VIDIOC_STREAMON, &type), streamerr);
        if (context[i].display) {
            if (flag_enable_display == 0) {
                // trig vsync
                display_commit_buffer(context[i].display_buffers[0], context[i].offset_x, context[i].offset_y);
            }
            flag_enable_display = 1;
            d = context[i].plane->display;
            display_fd = d->fd;
            d->frame_count = 0;
        }
        continue;
        streamerr:
        for (unsigned j = 0; j < i; j++) {
            ioctl(context[i].video_fd, VIDIOC_STREAMOFF, &type);
        }
        return -1;
    }

    bool display_commit_pending = flag_enable_display != 0;
    struct pollfd fds[num + flag_enable_display];
    while (1) {
        for (unsigned i = 0; i < num; i++) {
            fds[i].fd = context[i].video_fd;
            fds[i].events = POLLIN | POLLPRI;
            fds[i].revents = 0;
        }
        if (flag_enable_display) {
            fds[num].fd = display_fd;
            fds[num].events = POLLIN | POLLPRI;
            fds[num].revents = 0;
        }
        int ret = poll(fds, num + flag_enable_display, 1000);
        if (((ret < 0) && (errno == EINTR)) || (ret == 0)) {
            continue;
        } else if (ret < 0) {
            pr("poll error %d(%s)", errno, strerror(errno));
            break;
        }

        for (unsigned i = 0; i < num; i++) {
            context[i].vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (context[i].display) {
                context[i].vbuffer.memory = V4L2_MEMORY_DMABUF;
            } else {
                context[i].vbuffer.memory = V4L2_MEMORY_MMAP;
            }
            if (fds[i].revents) {
                if (context[i].flag_dqbuf) {
                    // drop frame
                    if (ioctl(context[i].video_fd, VIDIOC_DQBUF, &context[i].vbuffer)) {
                        continue;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: DQBUF index %u dmabuf %d --", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    context[i].frame_count += 1;
                    if (context[i].flag_dump) {
                        dump_file(&context[i], i);
                        context[i].flag_dump = false;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: QBUF  index %u dmabuf %d --", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer);
                    continue;
                }
                context[i].wp = (context[i].wp + 1) % DRM_BUFFERING;
                if (context[i].buffer_hold[context[i].wp] >= 0) {
                    // QBUF displayed frame
                    context[i].vbuffer.index = context[i].buffer_hold[context[i].wp];
                    if (context[i].vbuffer.memory == V4L2_MEMORY_DMABUF) {
                        context[i].vbuffer.m.fd = context[i].buffers[context[i].vbuffer.index].fd;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: QBUF  index %u dmabuf %d", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer);
                }
                // DQBUF
                if (ioctl(context[i].video_fd, VIDIOC_DQBUF, &context[i].vbuffer) < 0) {
                    // error, skip this frame
                    continue;
                }
                #if DEBUG_SEQ
                pr("DEBUG: DQBUF index %u dmabuf %d", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                #endif
                if (context[i].flag_dump) {
                    dump_file(&context[i], i);
                    context[i].flag_dump = false;
                }
                context[i].frame_count += 1;
                context[i].buffer_hold[context[i].wp] = context[i].vbuffer.index;
                context[i].flag_dqbuf = true;
            }
        }

        const bool displayed = flag_enable_display && fds[num].revents &&
                               (!event_driven || display_commit_pending);
        int ch = 0;
        if (handler) {
            ch = handler(context, displayed);
            switch (ch) {
                case -1:
                case 'q': goto streamoff;
                case 'd':
                    dump_count += 1;
                    for (unsigned i = 0; i < num; i++) {
                        context[i].flag_dump = true;
                    }
                    break;
                default:
                    break;
            }
        }

        if (!event_driven) {
            if (flag_enable_display && fds[num].revents) {
                // display
                bool flag_check_source = false;
                for (unsigned i = 0; i < num; i++) {
                    if ((context[i].buffer_hold[context[i].wp] >= 0) &&
                        context[i].display) {
                        flag_check_source = true;
                        break;
                    }
                }
                if (!flag_check_source) {
                    // skip
                    continue;
                }
                display_handle_vsync(d);
                for (unsigned i = 0; i < num; i++) {
                    if (!context[i].display ||
                        context[i].buffer_hold[context[i].wp] < 0) {
                        continue;
                    }
                    CKE(display_update_buffer(
                        context[i].display_buffers[
                            context[i].buffer_hold[context[i].wp]],
                        context[i].offset_x, context[i].offset_y
                    ), streamoff);
                    context[i].flag_dqbuf = false;
                }
                CKE(display_commit(d), streamoff);
                d->frame_count += 1;
            }
        } else {
            if (displayed) {
                display_handle_vsync(d);
                display_commit_pending = false;
            }

            if (flag_enable_display && !display_commit_pending &&
                v4l2_drm_have_pending_display_update(d, context, num)) {
                CKE(v4l2_drm_commit_pending_display_update(d, context, num),
                    streamoff);
                display_commit_pending = true;
            }
        }
    }
    streamoff:
    for (unsigned i = 0; i < num; i++) {
        v4l2_drm_stop(&context[i]);
    }
    return 0;
}

int v4l2_drm_run(struct v4l2_drm_context context[], unsigned num,
                 v4l2_drm_handler handler)
{
    return v4l2_drm_run_impl(context, num, handler, false);
}

int v4l2_drm_run_event_driven(struct v4l2_drm_context context[], unsigned num,
                              v4l2_drm_handler handler)
{
    return v4l2_drm_run_impl(context, num, handler, true);
}

bool v4l2_drm_run_v4l2_2_drm_need_run = 1;

static int v4l2_drm_run_v4l2_2_drm_have_data_to_display(
    const struct display* d,
    const struct v4l2_drm_context context[],
    unsigned num)
{
    if (d->lvgl_disp_buffer || d->osd_disp_buffer)
        return 1;

    for (unsigned i = 0; i < num; i++) {
        if (context[i].buffer_hold[context[i].wp] >= 0 &&
            context[i].display)
            return 1;
    }
    return 0; // no data
}

int v4l2_drm_run_v4l2_2_drm(struct v4l2_drm_context context[], unsigned num, v4l2_drm_handler handler) {
    int flag_enable_display = 0;
    int display_fd;
    struct display* d = NULL;
    for (unsigned i = 0; i < num; i++) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (context[i].display) {
            CKE(ioctl(context[i].video_fd, VIDIOC_STREAMON, &type), streamerr);
            if (flag_enable_display == 0) {
                // trig vsync
                display_commit_buffer(context[i].display_buffers[0], context[i].offset_x, context[i].offset_y);
                flag_enable_display = 1;
                d = context[i].plane->display;
                display_fd = d->fd;
                d->frame_count = 0;
            }

        }
        continue;
        streamerr:
        for (unsigned j = 0; j < i; j++) {
            ioctl(context[i].video_fd, VIDIOC_STREAMOFF, &type);
        }
        return -1;
    }

    struct pollfd fds[num + flag_enable_display];
    while (v4l2_drm_run_v4l2_2_drm_need_run) {
        for (unsigned i = 0; i < num; i++) {
            fds[i].fd = context[i].video_fd;
            if(context[i].display)
                fds[i].events = POLLIN | POLLPRI;
            else
                fds[i].events = 0;
            fds[i].revents = 0;
        }
        if (flag_enable_display) {
            fds[num].fd = display_fd;
            fds[num].events = POLLIN | POLLPRI;
            fds[num].revents = 0;
        }
        int ret = poll(fds, num + flag_enable_display, 1000);
        if (((ret < 0) && (errno == EINTR)) || (ret == 0)) {
            continue;
        } else if (ret < 0) {
            pr("poll error %d(%s)", errno, strerror(errno));
            break;
        }

        for (unsigned i = 0; i < num; i++) {
            context[i].vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (context[i].display) {
                context[i].vbuffer.memory = V4L2_MEMORY_DMABUF;
            } else {
                context[i].vbuffer.memory = V4L2_MEMORY_MMAP;
            }
            if (fds[i].revents) {
                if (context[i].flag_dqbuf) {
                    // drop frame
                    if (ioctl(context[i].video_fd, VIDIOC_DQBUF, &context[i].vbuffer)) {
                        continue;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: DQBUF index %u dmabuf %d --", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    context[i].frame_count += 1;
                    if (context[i].flag_dump) {
                        dump_file(&context[i], i);
                        context[i].flag_dump = false;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: QBUF  index %u dmabuf %d --", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer);
                    continue;
                }
                context[i].wp = (context[i].wp + 1) % DRM_BUFFERING;
                if (context[i].buffer_hold[context[i].wp] >= 0) {
                    // QBUF displayed frame
                    context[i].vbuffer.index = context[i].buffer_hold[context[i].wp];
                    if (context[i].vbuffer.memory == V4L2_MEMORY_DMABUF) {
                        context[i].vbuffer.m.fd = context[i].buffers[context[i].vbuffer.index].fd;
                    }
                    #if DEBUG_SEQ
                    pr("DEBUG: QBUF  index %u dmabuf %d", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                    #endif
                    ioctl(context[i].video_fd, VIDIOC_QBUF, &context[i].vbuffer);
                }
                // DQBUF
                if (ioctl(context[i].video_fd, VIDIOC_DQBUF, &context[i].vbuffer) < 0) {
                    // error, skip this frame
                    continue;
                }
                #if DEBUG_SEQ
                pr("DEBUG: DQBUF index %u dmabuf %d", context[i].vbuffer.index, context[i].vbuffer.m.fd);
                #endif
                if (context[i].flag_dump) {
                    dump_file(&context[i], i);
                    context[i].flag_dump = false;
                }
                context[i].frame_count += 1;
                context[i].buffer_hold[context[i].wp] = context[i].vbuffer.index;
                context[i].flag_dqbuf = true;
            }
        }

        int ch = 0;
        if (handler) {
            ch = handler(context, flag_enable_display && fds[num].revents);
            switch (ch) {
                case -1:
                case 'q': goto streamoff;
                case 'd':
                    dump_count += 1;
                    for (unsigned i = 0; i < num; i++) {
                        context[i].flag_dump = true;
                    }
                    break;
                default:
                    break;
            }
        }

        if (flag_enable_display && fds[num].revents) {
            if (!v4l2_drm_run_v4l2_2_drm_have_data_to_display(
                    d, context, num)) {
                usleep(10000);
                continue;
            }
            display_handle_vsync(d);
            for (unsigned i = 0; i < num; i++) {
                if (!context[i].display ||
                    context[i].buffer_hold[context[i].wp] < 0) {
                    continue;
                }
                CKE(display_update_buffer(
                    context[i].display_buffers[
                        context[i].buffer_hold[context[i].wp]],
                    context[i].offset_x, context[i].offset_y
                ), streamoff);
                context[i].flag_dqbuf = false;
            }
            if (d->osd_disp_buffer) {
                display_update_buffer(d->osd_disp_buffer, 0, 0);
                d->osd_disp_buffer = NULL;
            }
            if (d->lvgl_disp_buffer) {
                display_update_buffer(d->lvgl_disp_buffer, 0, 0);
                d->lvgl_disp_buffer = NULL;
            }

            CKE(display_commit(d), streamoff);
            d->frame_count += 1;
        }
    }
    streamoff:
    for (unsigned i = 0; i < num; i++) {
        v4l2_drm_stop(&context[i]);
    }
    return 0;
}
int v4l2_drm_start(const struct v4l2_drm_context* context) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(context->video_fd, VIDIOC_STREAMON, &type);
}

/* Release MMAP+EXPBUF buffers (scene_switch / non-display path). */
static void v4l2_drm_release_mmap_buffers(struct v4l2_drm_context *ctx)
{
    unsigned j;

    if (!ctx->buffers || ctx->video_fd < 0) {
        return;
    }

    for (j = 0; j < ctx->buffer_num; j++) {
        struct v4l2_buffer vb;
        size_t len = 0;

        memset(&vb, 0, sizeof(vb));
        vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vb.memory = V4L2_MEMORY_MMAP;
        vb.index = j;
        if (ioctl(ctx->video_fd, VIDIOC_QUERYBUF, &vb) == 0) {
            len = vb.length;
        } else if (ctx->vbuffer.length) {
            len = ctx->vbuffer.length;
        }

        if (ctx->buffers[j].mmap && ctx->buffers[j].mmap != MAP_FAILED && len) {
            munmap(ctx->buffers[j].mmap, len);
        }
        ctx->buffers[j].mmap = NULL;

        if (ctx->buffers[j].fd >= 0) {
            close(ctx->buffers[j].fd);
            ctx->buffers[j].fd = -1;
        }
    }
}

int v4l2_drm_stop(const struct v4l2_drm_context *context)
{
    struct v4l2_drm_context *ctx = (struct v4l2_drm_context *)context;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret = 0;

    if (!ctx || ctx->video_fd < 0) {
        return -1;
    }

    if (!ctx->display && ctx->buffers) {
        v4l2_drm_release_mmap_buffers(ctx);
    } else if (ctx->display && ctx->buffers) {
        /* Display path: mmap/fd owned by display_buffer; do not munmap/close here. */
        for (unsigned j = 0; j < ctx->buffer_num; j++) {
            ctx->buffers[j].mmap = NULL;
        }
    }

    ret = ioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type);
    close(ctx->video_fd);
    ctx->video_fd = -1;

    return ret;
}

int v4l2_drm_dump(struct v4l2_drm_context* context, int timeout) {
    struct pollfd pf = {
        .events = POLLIN | POLLPRI,
        .fd = context->video_fd,
        .revents = 0
    };
    int ret;
    retry:
    ret = poll(&pf, 1, timeout);
    if ((ret < 0) && (errno == EINTR)) {
        // try again
        goto retry;
    }
    if (ret <= 0) {
        return ret;
    }
    return ioctl(context->video_fd, VIDIOC_DQBUF, &context->vbuffer);
}

/* Latest-wins dequeue: wait for readiness, then drain the driver's done-queue
 * FIFO (DQBUF returns the oldest first), requeuing every older buffer
 * immediately (QBUF) and keeping only the newest one. O_NONBLOCK fd means an
 * empty queue surfaces as a failed DQBUF (EAGAIN), giving a clean loop bound.
 * Stale frames are recycled instead of being handed to the consumer. */
int v4l2_drm_dump_latest(struct v4l2_drm_context* context, int timeout)
{
    struct pollfd pf = {
        .events = POLLIN | POLLPRI,
        .fd = context->video_fd,
        .revents = 0
    };
    int ret;
retry:
    ret = poll(&pf, 1, timeout);
    if ((ret < 0) && (errno == EINTR)) {
        goto retry;
    }
    if (ret <= 0) {
        return ret;
    }

    struct v4l2_buffer held = context->vbuffer; /* newest so far */
    bool have = false;
    for (;;) {
        struct v4l2_buffer vb = context->vbuffer; /* inherits type/memory */
        if (ioctl(context->video_fd, VIDIOC_DQBUF, &vb) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* done queue drained */
            return -1;
        }
        if (have && ioctl(context->video_fd, VIDIOC_QBUF, &held) < 0) {
            /* vb is not retained by the caller on this error path. */
            (void)ioctl(context->video_fd, VIDIOC_QBUF, &vb);
            return -1;
        }
        held = vb;
        have = true;
    }
    if (!have)
        return -1;
    context->vbuffer = held;
    return 0;
}

int v4l2_drm_dump_release(struct v4l2_drm_context* context) {
    return ioctl(context->video_fd, VIDIOC_QBUF, &context->vbuffer);
}
