/*
 * isp_lcd_preview — universal live camera preview for the K230.
 *
 * Design goals
 * ------------
 * 1. Universal across configs that attach the same LCD: it auto-detects
 *    the display connector and its native geometry at runtime instead of
 *    hard-coding a panel size, so the same binary serves multiple boards
 *    and multiple sensor modes without a rebuild.
 * 2. Core-agnostic: it is plain userspace built against the v4l2-drm and
 *    display libraries, so it runs identically on the scalar (small C908)
 *    and RVV (big) core. It does not touch the ISP daemon, tuning files,
 *    or any application code — it is a preview utility only.
 * 3. Destroys the earlier "pink column" class of failure by replicating the
 *    confirmed-working portrait-DSI geometry from the tag apps' display
 *    thread: landscape capture buffer derived from the panel, 90-degree
 *    rotation, explicit destination rectangle from the panel, event-driven
 *    updates capped at a sane rate. Unlike the generic `v4l2-drm` CLI (which
 *    cannot decouple the portrait destination from the source buffer), this
 *    tool sets the destination rectangle directly.
 *
 * HDMI vs DSI (different display paths)
 * ------------------------------------
 * The connector is queried at startup (via display_is_hdmi()). DSI panels
 * are typically portrait (e.g. 480x800) and need the 90-degree rotation plus
 * a portrait destination; HDMI monitors are landscape and use a passthrough
 * rectangle. This tool handles both, mirroring how the tag apps branch on
 * connector type — it never conflates the two paths.
 *
 * Build: buildroot package (see ../Config.in, ../isp_lcd_preview.mk).
 * Usage: isp_lcd_preview [fps]     (optional display cap, default 30)
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <display.h>
#include <v4l2-drm.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Frame handler: stop on signal or 'q'. Return value 0 = keep going. */
static int on_frame(struct v4l2_drm_context *ctx, bool displayed)
{
    (void)ctx;
    (void)displayed;
    char key = '\0';
    if (g_stop || (read(STDIN_FILENO, &key, 1) == 1 && (key == 'q' || key == 'Q')))
        return 'q';
    return 0;
}

int main(int argc, char **argv)
{
    unsigned fps = 30;
    if (argc > 1)
        fps = (unsigned)strtoul(argv[1], NULL, 10);
    if (fps == 0)
        fps = 30;

    struct display *display = display_init_refresh(0, fps);
    if (!display) {
        fprintf(stderr, "isp_lcd_preview: display_init_refresh failed\n");
        return 1;
    }

    struct v4l2_drm_context context;
    v4l2_drm_default_context(&context);
    context.device = 1;                 /* camera node */
    context.video_format = V4L2_PIX_FMT_NV12;
    context.display_format = 0;

    if (display_is_hdmi(display)) {
        /* HDMI: landscape passthrough; dest == source, no rotation. */
        context.width = display->width;
        context.height = display->height;
        context.display_width = display->width;
        context.display_height = display->height;
        context.drm_rotation = rotation_0;
        fprintf(stderr, "isp_lcd_preview: HDMI %ux%u passthrough\n",
                display->width, display->height);
    } else if (display->width < display->height) {
        /* Portrait DSI panel (e.g. 480x800). Landscape capture buffer
         * (panel long side x short side), 90 CW rotation, explicit
         * portrait destination. Replicates the tag apps' confirmed
         * working geometry. */
        context.width = display->height;             /* landscape capture W */
        context.height = display->width;             /* landscape capture H */
        context.drm_rotation = rotation_90;
        context.display_width = display->width;
        context.display_height = display->height;
        fprintf(stderr, "isp_lcd_preview: portrait DSI %ux%u, rotation 90\n",
                display->width, display->height);
    } else {
        /* Landscape DSI (rare) — passthrough like HDMI. */
        context.width = display->width;
        context.height = display->height;
        context.display_width = display->width;
        context.display_height = display->height;
        context.drm_rotation = rotation_0;
        fprintf(stderr, "isp_lcd_preview: landscape DSI %ux%u passthrough\n",
                display->width, display->height);
    }

    /* Optional cap on event-driven display submissions (panel scanout may be
     * 60 Hz, but each submission costs CPU on a single core). */
    context.max_display_fps = fps;

    if (v4l2_drm_setup(&context, 1, &display) != 0) {
        fprintf(stderr, "isp_lcd_preview: v4l2_drm_setup failed\n");
        display_exit(display);
        return 1;
    }

    fprintf(stderr,
            "isp_lcd_preview: source %ux%u -> dest %ux%u @<=%u fps; press q+Enter or Ctrl-C to exit\n",
            context.width, context.height,
            context.display_width, context.display_height, fps);

    /* Non-blocking stdin for 'q'. */
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = v4l2_drm_run_event_driven(&context, 1, on_frame);
    if (flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, flags);
    display_exit(display);
    return rc < 0 ? 1 : 0;
}
