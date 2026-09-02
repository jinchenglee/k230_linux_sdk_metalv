/* k230_osd.cc -- shared, zero-copy OSD overlay queue for K230 apps. */
#include "k230_osd.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#include "thead.h"

namespace {

enum class buffer_state : uint8_t {
    free,
    front,
    pending,
    drawing,
    ready,
};

} // namespace

struct k230_osd {
    struct display *display = nullptr;
    struct display_plane *plane = nullptr;
    k230_osd_config config{};
    k230_osd_mode mode = K230_OSD_MODE_FAST;
    k230_osd_mode drawing_mode = K230_OSD_MODE_FAST;

    struct display_buffer *buffers[K230_OSD_BUFFER_COUNT]{};
    buffer_state states[K230_OSD_BUFFER_COUNT]{};
    cv::Mat fast_view[K230_OSD_BUFFER_COUNT];

    /* The event callback is invoked before display_handle_vsync(). Therefore
     * pending_idx is selected by the preceding callback and only becomes
     * FRONT when the next displayed callback arrives. */
    int front_idx = 0;
    int pending_idx = -1;
    int drawing_idx = -1;
    int ready_idx = -1;

    uint64_t buffer_generation[K230_OSD_BUFFER_COUNT]{};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> displayed_generation{0};
    std::atomic<uint64_t> dropped_frames{0};

    /* SLOW_ROTATE draws landscape here, then rotates directly into the mapped
     * DRM buffer. No frame-sized temporary or memcpy is needed. */
    cv::Mat slow_landscape;
    cv::Mat empty_view;

    mutable std::mutex mutex;
};

static bool make_fast_view(k230_osd *osd, int idx)
{
    struct display_buffer *buffer = osd->buffers[idx];
    if (!buffer || !buffer->map || buffer->stride < buffer->width * 4u)
        return false;

    const uint64_t required =
        static_cast<uint64_t>(buffer->stride) * buffer->height;
    if (required > buffer->size)
        return false;

    osd->fast_view[idx] =
        cv::Mat(static_cast<int>(buffer->height),
                static_cast<int>(buffer->width), CV_8UC4, buffer->map,
                static_cast<size_t>(buffer->stride));
    return !osd->fast_view[idx].empty();
}

static void clear_drawing_buffer(k230_osd *osd, int idx,
                                 k230_osd_mode mode)
{
    if (mode == K230_OSD_MODE_FAST) {
        /* Clear padding too. The whole allocation is flushed at publish, and
         * memset is cheaper than a per-pixel OpenCV scalar loop. */
        std::memset(osd->buffers[idx]->map, 0, osd->buffers[idx]->size);
    } else {
        osd->slow_landscape.setTo(cv::Scalar(0, 0, 0, 0));
    }
}

static int acquire_drawing_buffer(k230_osd *osd, k230_osd_mode *mode)
{
    std::lock_guard<std::mutex> lock(osd->mutex);

    if (osd->drawing_idx >= 0) {
        osd->drawing_mode = osd->mode;
        *mode = osd->drawing_mode;
        return osd->drawing_idx;
    }

    int idx = -1;
    for (int i = 0; i < K230_OSD_BUFFER_COUNT; ++i) {
        if (osd->states[i] == buffer_state::free) {
            idx = i;
            break;
        }
    }

    /* If the producer outruns vsync, repaint the not-yet-staged READY buffer
     * instead of blocking detection or increasing latency. */
    if (idx < 0 && osd->ready_idx >= 0) {
        idx = osd->ready_idx;
        osd->ready_idx = -1;
        osd->dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }

    if (idx < 0)
        return -1; /* only possible with API misuse or external plane access */

    osd->states[idx] = buffer_state::drawing;
    osd->drawing_idx = idx;
    osd->drawing_mode = osd->mode;
    *mode = osd->drawing_mode;
    return idx;
}

k230_osd *k230_osd_create(struct display *display,
                           const k230_osd_config *config)
{
    if (!display || !config || config->width == 0 || config->height == 0)
        return nullptr;

    struct display_plane *plane =
        display_get_plane(display, DRM_FORMAT_ARGB8888);
    if (!plane)
        return nullptr;

    k230_osd *osd = new (std::nothrow) k230_osd{};
    if (!osd) {
        display_free_plane(plane);
        return nullptr;
    }

    osd->display = display;
    osd->plane = plane;
    osd->config = *config;
    osd->mode = config->mode;
    osd->drawing_mode = config->mode;

    for (int i = 0; i < K230_OSD_BUFFER_COUNT; ++i) {
        osd->buffers[i] =
            display_allocate_buffer(plane, config->width, config->height);
        if (!osd->buffers[i] || !make_fast_view(osd, i)) {
            k230_osd_destroy(osd);
            return nullptr;
        }
        std::memset(osd->buffers[i]->map, 0, osd->buffers[i]->size);
        osd->states[i] = buffer_state::free;
    }

    osd->states[0] = buffer_state::front;
    osd->front_idx = 0;

    /* A clockwise rotation into HxW scanout needs a WxH landscape source. */
    osd->slow_landscape =
        cv::Mat(static_cast<int>(config->width),
                static_cast<int>(config->height), CV_8UC4,
                cv::Scalar(0, 0, 0, 0));
    if (osd->slow_landscape.empty()) {
        k230_osd_destroy(osd);
        return nullptr;
    }

    return osd;
}

void k230_osd_destroy(k230_osd *osd)
{
    if (!osd)
        return;

    if (osd->display && osd->display->osd_disp_buffer) {
        for (int i = 0; i < K230_OSD_BUFFER_COUNT; ++i) {
            if (osd->display->osd_disp_buffer == osd->buffers[i]) {
                osd->display->osd_disp_buffer = nullptr;
                break;
            }
        }
    }

    for (int i = 0; i < K230_OSD_BUFFER_COUNT; ++i) {
        if (osd->buffers[i]) {
            display_free_buffer(osd->buffers[i]);
            osd->buffers[i] = nullptr;
        }
    }
    if (osd->plane)
        display_free_plane(osd->plane);
    delete osd;
}

const struct display_buffer *k230_osd_front_buffer(const k230_osd *osd)
{
    return osd ? osd->buffers[osd->front_idx] : nullptr;
}

void k230_osd_set_mode(k230_osd *osd, k230_osd_mode mode)
{
    if (!osd)
        return;
    std::lock_guard<std::mutex> lock(osd->mutex);
    osd->mode = mode;
}

void k230_osd_prepare(k230_osd *osd)
{
    if (!osd)
        return;

    k230_osd_mode mode;
    const int idx = acquire_drawing_buffer(osd, &mode);
    if (idx >= 0)
        clear_drawing_buffer(osd, idx, mode);
}

cv::Mat &k230_osd_begin(k230_osd *osd)
{
    if (!osd)
        throw std::invalid_argument("k230_osd_begin: null osd");

    int idx;
    k230_osd_mode mode;
    {
        std::lock_guard<std::mutex> lock(osd->mutex);
        idx = osd->drawing_idx;
        mode = osd->drawing_mode;
    }

    /* Be forgiving if a caller omitted prepare(). The documented fast path
     * still calls prepare early so the clear overlaps hardware detection. */
    if (idx < 0) {
        k230_osd_prepare(osd);
        std::lock_guard<std::mutex> lock(osd->mutex);
        idx = osd->drawing_idx;
        mode = osd->drawing_mode;
    }

    if (idx < 0)
        return osd->empty_view;
    return mode == K230_OSD_MODE_FAST ? osd->fast_view[idx]
                                      : osd->slow_landscape;
}

void k230_osd_publish(k230_osd *osd)
{
    if (!osd)
        return;

    int idx;
    k230_osd_mode mode;
    {
        std::lock_guard<std::mutex> lock(osd->mutex);
        idx = osd->drawing_idx;
        mode = osd->drawing_mode;
    }
    if (idx < 0)
        return;

    struct display_buffer *buffer = osd->buffers[idx];
    if (mode == K230_OSD_MODE_SLOW_ROTATE) {
        /* Rotate straight into the pitch-aware mapped view: no temp and no
         * full-frame memcpy. */
        cv::rotate(osd->slow_landscape, osd->fast_view[idx],
                   cv::ROTATE_90_CLOCKWISE);
    }

    thead_csi_dcache_clean_invalid_range(buffer->map, buffer->size);

    std::lock_guard<std::mutex> lock(osd->mutex);
    if (osd->drawing_idx != idx ||
        osd->states[idx] != buffer_state::drawing)
        return;

    if (osd->ready_idx >= 0) {
        osd->states[osd->ready_idx] = buffer_state::free;
        osd->dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t generation =
        osd->generation.fetch_add(1, std::memory_order_release) + 1;
    osd->buffer_generation[idx] = generation;
    osd->states[idx] = buffer_state::ready;
    osd->ready_idx = idx;
    osd->drawing_idx = -1;
}

void k230_osd_on_frame(k230_osd *osd, bool displayed)
{
    if (!osd || !displayed)
        return;

    std::lock_guard<std::mutex> lock(osd->mutex);

    /* The event now readable is completion of the atomic commit staged by
     * the preceding callback. Only now is the former FRONT safe for CPU. */
    if (osd->pending_idx >= 0) {
        if (osd->front_idx >= 0)
            osd->states[osd->front_idx] = buffer_state::free;
        osd->front_idx = osd->pending_idx;
        osd->states[osd->front_idx] = buffer_state::front;
        osd->pending_idx = -1;
        osd->displayed_generation.store(
            osd->buffer_generation[osd->front_idx],
            std::memory_order_release);
    }

    if (osd->ready_idx < 0)
        return;

    const int idx = osd->ready_idx;
    osd->ready_idx = -1;
    osd->states[idx] = buffer_state::pending;
    osd->pending_idx = idx;

    /* The v4l2/DRM loop consumes this immediately after the callback and
     * includes it in the next atomic commit. Only this display thread writes
     * the field, so no cross-thread pointer race is introduced here. */
    osd->display->osd_disp_buffer = osd->buffers[idx];
}

uint64_t k230_osd_generation(const k230_osd *osd)
{
    return osd ? osd->generation.load(std::memory_order_acquire) : 0;
}

uint64_t k230_osd_displayed_generation(const k230_osd *osd)
{
    return osd ? osd->displayed_generation.load(std::memory_order_acquire) : 0;
}

uint64_t k230_osd_dropped_frames(const k230_osd *osd)
{
    return osd ? osd->dropped_frames.load(std::memory_order_relaxed) : 0;
}

bool k230_osd_lcd_fastpath(const k230_osd *osd)
{
    return osd && osd->config.lcd_fastpath;
}
