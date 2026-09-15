#include "benchmark.h"

#include <aruco_nano/aruco_nano.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/aruco_dictionary.hpp>

namespace apriltag_bench {
namespace {

std::uint64_t monotonic_raw_ns()
{
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(time.tv_nsec);
}


class ArucoNanoBackend final : public Backend {
public:
    explicit ArucoNanoBackend(const BenchmarkConfig& config)
        : factor_(config.factor_value)
    {
        parameters_.dicts = {cv::aruco::getPredefinedDictionary(
            cv::aruco::DICT_APRILTAG_36h11)};
        parameters_.errorCorrectionRate =
            config.aruco_error_correction_rate;
        parameters_.maxErroneousBitsInBorderRate =
            config.aruco_border_error_rate;
        parameters_.detectInvertedMarker = false;
        normalized_.reserve(kMaxDetections);
    }

    BackendKind kind() const override { return BackendKind::ArucoNano; }
    const char* name() const override { return backend_name(kind()); }
    void set_capture_detections(bool capture) override { capture_ = capture; }
    const std::vector<Detection>& detections() const override
    {
        return normalized_;
    }

    DetectionResult detect(const PreparedImage& image) override
    {
        const cv::Mat input = image_view(image);
        const std::uint64_t scale_start = monotonic_raw_ns();
        const cv::Mat& detector_input = scaled_input(input);
        const std::uint64_t input_scale_ns =
            factor_ == 1.0 ? 0 : monotonic_raw_ns() - scale_start;
        const std::uint64_t detector_start = monotonic_raw_ns();
        const auto markers =
            aruco_nano::MarkerDetector::detect(detector_input, parameters_);
        const std::uint64_t detector_ns = monotonic_raw_ns() - detector_start;
        const int count = validate_detection_count(
            checked_detection_count(markers.size()));
        if (capture_) normalized_.clear();
        DetectionChecksum checksum(static_cast<std::size_t>(count));
        for (const auto& marker : markers) {
            if (marker.size() != 4 || marker.id < 0) {
                throw std::runtime_error("ArUco Nano returned an invalid marker");
            }
            Detection normalized;
            normalized.id = static_cast<std::uint64_t>(marker.id);
            normalized.margin = std::numeric_limits<double>::quiet_NaN();
            for (int corner = 0; corner < 4; ++corner) {
                normalized.corners[corner * 2] = marker[corner].x * factor_;
                normalized.corners[corner * 2 + 1] = marker[corner].y * factor_;
                normalized.center[0] += normalized.corners[corner * 2] * 0.25;
                normalized.center[1] += normalized.corners[corner * 2 + 1] * 0.25;
            }
            checksum.add(normalized);
            if (capture_) normalized_.push_back(normalized);
        }
        return {count, checksum.value(), {true, input_scale_ns, detector_ns}};
    }

private:
    static int checked_detection_count(std::size_t count)
    {
        if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("ArUco Nano detection count overflows int");
        }
        return static_cast<int>(count);
    }

    static cv::Mat image_view(const PreparedImage& image)
    {
        if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            image.height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("image geometry exceeds ArUco Nano limits");
        }
        return cv::Mat(static_cast<int>(image.height), static_cast<int>(image.width),
                       CV_8UC1, const_cast<std::uint8_t*>(image.pixels.data()),
                       image.stride);
    }

    const cv::Mat& scaled_input(const cv::Mat& input)
    {
        if (factor_ == 1.0) return input;
        const int width = std::max(1, cvRound(input.cols / factor_));
        const int height = std::max(1, cvRound(input.rows / factor_));
        cv::resize(input, scaled_, {width, height}, 0, 0, cv::INTER_AREA);
        return scaled_;
    }

    double factor_;
    aruco_nano::DetectorParameters parameters_;
    cv::Mat scaled_;
    bool capture_ = false;
    std::vector<Detection> normalized_;
};

}  // namespace

std::unique_ptr<Backend> make_aruco_nano_backend(const BenchmarkConfig& config)
{
    return std::unique_ptr<Backend>(new ArucoNanoBackend(config));
}

}  // namespace apriltag_bench
