#include "benchmark.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace apriltag_bench {
namespace {

int coordinate(double value)
{
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        throw std::runtime_error("detection coordinate is not drawable");
    }
    return cvRound(value);
}

cv::Scalar edge_color(std::size_t index)
{
    static const cv::Scalar colors[] = {
        {32, 32, 255}, {32, 220, 32}, {255, 96, 32}, {220, 32, 220}};
    return colors[index % 4];
}

std::string output_name(BackendKind kind)
{
    return std::string(backend_key(kind)) + "-detections.png";
}

void write_png(const std::filesystem::path& path, const cv::Mat& image)
{
    try {
        if (!cv::imwrite(path.string(), image)) {
            throw std::runtime_error("OpenCV returned false");
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("cannot write visual dump " + path.string() +
                                 ": " + error.what());
    }
}

}  // namespace

void write_visual_dumps(const std::string& directory,
                        const PreparedImage& image,
                        const std::vector<VisualDump>& dumps)
{
    validate_image(image.width, image.height, image.stride, image.pixels.size());
    if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("image geometry exceeds OpenCV limits");
    }

    std::error_code error;
    const std::filesystem::path output(directory);
    std::filesystem::create_directories(output, error);
    if (error || !std::filesystem::is_directory(output, error) || error) {
        throw std::runtime_error("cannot create visual dump directory: " +
                                 directory);
    }

    const cv::Mat gray(static_cast<int>(image.height), static_cast<int>(image.width),
                       CV_8UC1, const_cast<std::uint8_t*>(image.pixels.data()),
                       image.stride);
    write_png(output / "input.png", gray);

    for (const VisualDump& dump : dumps) {
        cv::Mat overlay;
        cv::cvtColor(gray, overlay, cv::COLOR_GRAY2BGR);
        const int scale = std::max(1, std::min(overlay.cols, overlay.rows) / 500);
        const int thickness = std::max(1, scale * 2);
        std::ostringstream header;
        header << backend_name(dump.kind) << " | " << dump.detections.size()
               << (dump.detections.size() == 1 ? " detection" : " detections");
        cv::putText(overlay, header.str(), {8, 22 * scale},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55 * scale, {0, 255, 255},
                    thickness, cv::LINE_AA);

        if (dump.detections.empty()) {
            cv::putText(overlay, "No detections", {8, 48 * scale},
                        cv::FONT_HERSHEY_SIMPLEX, 0.55 * scale, {0, 255, 255},
                        thickness, cv::LINE_AA);
        }
        for (const Detection& detection : dump.detections) {
            cv::Point corners[4];
            for (int corner = 0; corner < 4; ++corner) {
                corners[corner] = {coordinate(detection.corners[corner * 2]),
                                   coordinate(detection.corners[corner * 2 + 1])};
            }
            for (int edge = 0; edge < 4; ++edge) {
                cv::line(overlay, corners[edge], corners[(edge + 1) % 4],
                         edge_color(static_cast<std::size_t>(edge)), thickness,
                         cv::LINE_AA);
            }
            const cv::Point center{coordinate(detection.center[0]),
                                   coordinate(detection.center[1])};
            cv::drawMarker(overlay, center, {0, 255, 255}, cv::MARKER_CROSS,
                           9 * scale, thickness, cv::LINE_AA);
            std::ostringstream label;
            label << "id " << detection.id << " m " << std::fixed
                  << std::setprecision(1) << detection.margin;
            cv::putText(overlay, label.str(), center + cv::Point(5, -5),
                        cv::FONT_HERSHEY_SIMPLEX, 0.45 * scale, {0, 255, 255},
                        thickness, cv::LINE_AA);
        }
        write_png(output / output_name(dump.kind), overlay);
    }
}

}  // namespace apriltag_bench
