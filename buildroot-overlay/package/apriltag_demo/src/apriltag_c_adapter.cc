/* Compatibility adapter from the official AprilRobotics C detector to the
 * compact C ABI consumed by the K230 live-demo capture/display shell.
 */
#include "apriltag.h"
#include "apriltag_c_adapter.h"

#include <apriltag/apriltag.h>
#include <apriltag/tag36h11.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

struct apriltag_c_detector {
    apriltag_detector_t* detector;
    apriltag_family_t* family;
    bool debug_enabled;
    bool debug_dumped;
};

static double factor_scale(int factor)
{
    switch (factor) {
    case 0: return 1.0;
    case 1: return 1.5;
    default: return 2.0;
    }
}

extern "C" void* apriltag_new(uint32_t min_blob_size)
{
    apriltag_c_detector* handle =
        static_cast<apriltag_c_detector*>(calloc(1, sizeof(*handle)));
    if (!handle) {
        return nullptr;
    }

    handle->detector = apriltag_detector_create();
    handle->family = tag36h11_create();
    if (!handle->detector || !handle->family) {
        if (handle->detector) {
            apriltag_detector_destroy(handle->detector);
        }
        if (handle->family) {
            tag36h11_destroy(handle->family);
        }
        free(handle);
        return nullptr;
    }

    handle->detector->qtp.min_cluster_pixels =
        static_cast<int>(std::min<uint32_t>(min_blob_size, INT_MAX));
    handle->detector->refine_edges = false;
    handle->detector->decode_sharpening = 0.0;
    apriltag_detector_add_family_bits(handle->detector, handle->family, 0);
    return handle;
}

extern "C" void apriltag_free(void* opaque)
{
    if (!opaque) {
        return;
    }
    apriltag_c_detector* handle =
        static_cast<apriltag_c_detector*>(opaque);
    apriltag_detector_destroy(handle->detector);
    tag36h11_destroy(handle->family);
    free(handle);
}

extern "C" int apriltag_c_configure(void* opaque,
                                      int threads,
                                      int bits_corrected,
                                      int refine_edges,
                                      double decode_sharpening)
{
    if (!opaque || threads < 1 || bits_corrected < 0 ||
        bits_corrected > 2 || !std::isfinite(decode_sharpening)) {
        return -1;
    }
    apriltag_c_detector* handle =
        static_cast<apriltag_c_detector*>(opaque);
    handle->detector->nthreads = threads;
    handle->detector->refine_edges = refine_edges != 0;
    handle->detector->decode_sharpening = decode_sharpening;

    apriltag_detector_clear_families(handle->detector);
    apriltag_detector_add_family_bits(
        handle->detector, handle->family, bits_corrected);
    return 0;
}

extern "C" int apriltag_set_debug_enabled(void* opaque, int enabled)
{
    if (!opaque) {
        return -1;
    }
    apriltag_c_detector* handle =
        static_cast<apriltag_c_detector*>(opaque);
    handle->debug_enabled = enabled != 0;
    if (handle->debug_enabled) {
        handle->debug_dumped = false;
    }
    return 0;
}

extern "C" int apriltag_configure_recovery(
    void* opaque, int enabled, double min_full_res_extent)
{
    (void)opaque;
    (void)enabled;
    (void)min_full_res_extent;
    return -1;
}

extern "C" int apriltag_set_debug_stage(void* opaque, int stage)
{
    return opaque && stage == 0 ? 0 : -1;
}

extern "C" int apriltag_get_debug_image(
    void* opaque, apriltag_debug_image_t* out)
{
    return opaque && out ? 0 : -1;
}

extern "C" int apriltag_get_decode_stats(
    void* opaque, apriltag_decode_stats_t* out)
{
    return opaque && out ? 0 : -1;
}

extern "C" int apriltag_get_decode_candidates(
    void* opaque, apriltag_decode_candidate_t* out, int max_out)
{
    return opaque && out && max_out > 0 ? 0 : -1;
}

extern "C" int apriltag_get_recovery_stats(
    void* opaque, apriltag_recovery_stats_t* out)
{
    (void)opaque;
    (void)out;
    return -1;
}

extern "C" int apriltag_get_recovery_candidates(
    void* opaque, apriltag_recovery_candidate_t* out, int max_out)
{
    return opaque && out && max_out > 0 ? 0 : -1;
}

extern "C" int apriltag_detect(
    void* opaque,
    const uint8_t* y,
    size_t width,
    size_t height,
    size_t stride,
    int factor,
    int mode,
    apriltag_det_t* out,
    int max_out)
{
    (void)mode;
    if (!opaque || !y || !out || max_out <= 0 || width == 0 ||
        height == 0 || stride < width || width > INT_MAX ||
        height > INT_MAX || stride > INT_MAX) {
        return -1;
    }

    apriltag_c_detector* handle =
        static_cast<apriltag_c_detector*>(opaque);
    handle->detector->quad_decimate = static_cast<float>(factor_scale(factor));
    handle->detector->debug =
        handle->debug_enabled && !handle->debug_dumped;

    // image_u8_t's geometry members are const in the upstream public API, so
    // construct this non-owning view in one aggregate initialization.
    image_u8_t image = {
        static_cast<int32_t>(width),
        static_cast<int32_t>(height),
        static_cast<int32_t>(stride),
        const_cast<uint8_t*>(y),
    };
    zarray_t* detections =
        apriltag_detector_detect(handle->detector, &image);
    if (!detections) {
        handle->detector->debug = false;
        return -1;
    }

    const int count = std::min(zarray_size(detections), max_out);
    for (int i = 0; i < count; ++i) {
        apriltag_detection_t* detection = nullptr;
        zarray_get(detections, i, &detection);
        out[i].id = static_cast<uint64_t>(detection->id);
        out[i].margin = detection->decision_margin;
        out[i].center[0] = detection->c[0];
        out[i].center[1] = detection->c[1];
        out[i].recovered = 0;
        out[i].visible_edges = 0;
        out[i].inferred_edges = 0;
        out[i].erasure_count = 0;
        out[i].corrected_bit_count = 0;
        out[i].geometry_residual = 0.0;
        out[i].recovery_group = 0;
        for (int corner = 0; corner < 4; ++corner) {
            out[i].corners[corner * 2] = detection->p[corner][0];
            out[i].corners[corner * 2 + 1] = detection->p[corner][1];
        }
    }

    apriltag_detections_destroy(detections);
    if (handle->detector->debug) {
        handle->debug_dumped = true;
        handle->detector->debug = false;
    }
    return count;
}
