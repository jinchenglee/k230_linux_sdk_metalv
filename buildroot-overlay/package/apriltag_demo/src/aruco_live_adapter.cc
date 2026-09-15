/* ArUco Nano/ArUco2 adapter for the existing K230 live-camera shell. */
#include "apriltag.h"
#include "aruco_live_adapter.h"

#include <aruco_nano/aruco_nano.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/aruco2.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace {

struct aruco_live_detector {
    explicit aruco_live_detector(uint32_t min_size)
    {
        const int checked_min_size = static_cast<int>(std::min<uint32_t>(
            min_size, static_cast<uint32_t>(std::numeric_limits<int>::max())));
        nano_parameters.minSize = checked_min_size;
        nano_parameters.dicts = {cv::aruco::getPredefinedDictionary(
            cv::aruco::DICT_APRILTAG_36h11)};
        nano_parameters.detectInvertedMarker = false;
        aruco2_parameters.minSize = checked_min_size;
        aruco2_parameters.detectColorMode = 0;
    }

    int backend = ARUCO_LIVE_BACKEND_NANO;
    aruco_nano::DetectorParameters nano_parameters;
    cv::aruco2::DetectionParameters aruco2_parameters;
    cv::Mat scaled;
};

double factor_scale(int factor)
{
    switch (factor) {
    case 0: return 1.0;
    case 1: return 1.5;
    case 2: return 2.0;
    default: return 0.0;
    }
}

template <typename Marker>
void copy_marker(const Marker& marker, double scale, apriltag_det_t& out)
{
    out.id = static_cast<uint64_t>(marker.id);
    out.margin = std::numeric_limits<double>::quiet_NaN();
    out.center[0] = 0.0;
    out.center[1] = 0.0;
    for (int corner = 0; corner < 4; ++corner) {
        out.corners[corner * 2] = marker[corner].x * scale;
        out.corners[corner * 2 + 1] = marker[corner].y * scale;
        out.center[0] += out.corners[corner * 2] * 0.25;
        out.center[1] += out.corners[corner * 2 + 1] * 0.25;
    }
}

} // namespace

extern "C" void* apriltag_new(uint32_t min_blob_size)
{
    if (min_blob_size == 0) {
        return nullptr;
    }
    try {
        return new aruco_live_detector(min_blob_size);
    } catch (...) {
        return nullptr;
    }
}

extern "C" void apriltag_free(void* opaque)
{
    delete static_cast<aruco_live_detector*>(opaque);
}

extern "C" int aruco_live_configure(void* opaque, int backend, int tolerant)
{
    if (!opaque ||
        (backend != ARUCO_LIVE_BACKEND_NANO &&
         backend != ARUCO_LIVE_BACKEND_ARUCO2) ||
        (tolerant != 0 && tolerant != 1)) {
        return -1;
    }
    auto* handle = static_cast<aruco_live_detector*>(opaque);
    handle->backend = backend;
    const double rate = tolerant ? 1.0 : 0.0;
    handle->nano_parameters.errorCorrectionRate = rate;
    handle->nano_parameters.maxErroneousBitsInBorderRate = rate;
    handle->aruco2_parameters.errorCorrectionRate = rate;
    handle->aruco2_parameters.maxErroneousBitsInBorderRate = rate;
    return 0;
}

extern "C" int apriltag_set_debug_enabled(void* opaque, int /*enabled*/)
{
    return opaque ? 0 : -1;
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

extern "C" int apriltag_detect(
    void* opaque,
    const uint8_t* y,
    size_t width,
    size_t height,
    size_t stride,
    int factor,
    int /*mode*/,
    apriltag_det_t* out,
    int max_out)
{
    const double scale = factor_scale(factor);
    if (!opaque || !y || !out || max_out <= 0 || width == 0 || height == 0 ||
        stride < width || width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        scale == 0.0) {
        return -1;
    }

    auto* handle = static_cast<aruco_live_detector*>(opaque);
    try {
        const cv::Mat input(static_cast<int>(height), static_cast<int>(width),
                            CV_8UC1, const_cast<uint8_t*>(y), stride);
        const cv::Mat* detector_input = &input;
        if (scale != 1.0) {
            const int scaled_width = std::max(1, cvRound(input.cols / scale));
            const int scaled_height = std::max(1, cvRound(input.rows / scale));
            cv::resize(input, handle->scaled,
                       cv::Size(scaled_width, scaled_height),
                       0, 0, cv::INTER_AREA);
            detector_input = &handle->scaled;
        }

        if (handle->backend == ARUCO_LIVE_BACKEND_NANO) {
            const auto markers = aruco_nano::MarkerDetector::detect(
                *detector_input, handle->nano_parameters);
            const int count = static_cast<int>(std::min<size_t>(
                markers.size(), static_cast<size_t>(max_out)));
            for (int i = 0; i < count; ++i) {
                if (markers[i].id < 0 || markers[i].size() != 4) {
                    return -1;
                }
                copy_marker(markers[i], scale, out[i]);
            }
            return count;
        }

        const auto markers = cv::aruco2::detectFiducialMarkers(
            *detector_input, cv::aruco2::DICT_APRILTAG_36h11,
            handle->aruco2_parameters);
        const int count = static_cast<int>(std::min<size_t>(
                markers.size(), static_cast<size_t>(max_out)));
        for (int i = 0; i < count; ++i) {
            if (markers[i].id < 0 || markers[i].size() != 4) {
                return -1;
            }
            copy_marker(markers[i], scale, out[i]);
        }
        return count;
    } catch (...) {
        return -1;
    }
}
