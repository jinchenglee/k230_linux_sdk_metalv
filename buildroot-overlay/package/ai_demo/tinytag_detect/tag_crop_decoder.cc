#include "tag_crop_decoder.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>

#include <aruco_nano/aruco_nano.h>
#include <opencv2/objdetect/aruco2.hpp>

extern "C"
{
#include <apriltag/apriltag.h>
#include <apriltag/tag36h11.h>
}

// apriltag-rvv's own C ABI (buildroot-overlay/package/apriltag_demo/src/
// apriltag.h) -- self-wraps in extern "C", so no extern "C" block needed
// here. Deliberately included after <apriltag/apriltag.h> above, matching
// the combination apriltag_demo's own apriltag_c_adapter.cc already uses
// (both headers are named "apriltag.h" but at different include paths --
// no collision as long as both search paths resolve correctly, which the
// existing apriltag_c_adapter.cc already proves).
#include "apriltag.h"

namespace
{
bool aruco_tolerant_mode()
{
    const char *env = std::getenv("TINYTAG_ARUCO_MODE");
    if (!env || std::string(env) == "strict")
        return false;
    if (std::string(env) == "tolerant")
        return true;
    throw std::runtime_error(
        "TINYTAG_ARUCO_MODE must be 'strict' or 'tolerant'");
}

void set_aruco_result_common(TagDetection &out, int id)
{
    out.id = id;
    // Neither ArUco API exposes these AprilTag-specific quality values.
    // NaN makes that explicit; dedupe then retains the first matching hit,
    // which comes from the higher-confidence TinyTag proposal.
    out.hamming = -1;
    out.decision_margin = std::numeric_limits<float>::quiet_NaN();
    out.center = cv::Point2f(0.0f, 0.0f);
}

class ArucoNanoDecoder final : public TagCropDecoder
{
public:
    explicit ArucoNanoDecoder(bool tolerant)
    {
        parameters_.minSize = 10;
        parameters_.dicts = {cv::aruco::getPredefinedDictionary(
            cv::aruco::DICT_APRILTAG_36h11)};
        parameters_.detectInvertedMarker = false;
        const double rate = tolerant ? 1.0 : 0.0;
        parameters_.errorCorrectionRate = rate;
        parameters_.maxErroneousBitsInBorderRate = rate;
    }

    std::vector<TagDetection> detect(const cv::Mat &crop) override
    {
        CV_Assert(crop.type() == CV_8UC1);
        const auto markers = aruco_nano::MarkerDetector::detect(crop, parameters_);
        std::vector<TagDetection> out;
        out.reserve(markers.size());
        for (const auto &marker : markers)
        {
            if (marker.id < 0 || marker.size() != 4)
                throw std::runtime_error("ArUco Nano returned an invalid marker");
            TagDetection detection{};
            set_aruco_result_common(detection, marker.id);
            for (int corner = 0; corner < 4; ++corner)
            {
                detection.corners[corner] = marker[corner];
                detection.center += marker[corner] * 0.25f;
            }
            out.push_back(detection);
        }
        return out;
    }

private:
    aruco_nano::DetectorParameters parameters_;
};

class Aruco2Decoder final : public TagCropDecoder
{
public:
    explicit Aruco2Decoder(bool tolerant)
    {
        parameters_.minSize = 10;
        parameters_.detectColorMode = 0;
        const double rate = tolerant ? 1.0 : 0.0;
        parameters_.errorCorrectionRate = rate;
        parameters_.maxErroneousBitsInBorderRate = rate;
    }

    std::vector<TagDetection> detect(const cv::Mat &crop) override
    {
        CV_Assert(crop.type() == CV_8UC1);
        const auto markers = cv::aruco2::detectFiducialMarkers(
            crop, cv::aruco2::DICT_APRILTAG_36h11, parameters_);
        std::vector<TagDetection> out;
        out.reserve(markers.size());
        for (const auto &marker : markers)
        {
            if (marker.id < 0 || marker.corners.size() != 4)
                throw std::runtime_error("ArUco2 returned an invalid marker");
            TagDetection detection{};
            set_aruco_result_common(detection, marker.id);
            for (int corner = 0; corner < 4; ++corner)
            {
                detection.corners[corner] = marker.corners[corner];
                detection.center += marker.corners[corner] * 0.25f;
            }
            out.push_back(detection);
        }
        return out;
    }

private:
    cv::aruco2::DetectionParameters parameters_;
};

apriltag_family_t *create_family(const std::string &name)
{
    if (name == "tag36h11")
        return tag36h11_create();
    throw std::runtime_error("AprilTagCDecoder: unsupported tag family '" + name + "'");
}

void destroy_family(const std::string &name, apriltag_family_t *fam)
{
    if (name == "tag36h11")
        tag36h11_destroy(fam);
}
} // namespace

AprilTagCDecoder::AprilTagCDecoder(const std::string &family_name, float quad_decimate)
    : detector_(nullptr), family_(nullptr), family_name_(family_name)
{
    apriltag_family_t *fam = create_family(family_name_);
    apriltag_detector_t *det = apriltag_detector_create();
    // Match apriltag_demo defaults while retaining factor 1 for TinyTag ROIs.
    apriltag_detector_add_family_bits(det, fam, 0);
    det->qtp.min_cluster_pixels = 25;
    det->quad_decimate = quad_decimate;
    det->refine_edges = false;
    det->decode_sharpening = 0.0;
    detector_ = det;
    family_ = fam;
}

AprilTagCDecoder::~AprilTagCDecoder()
{
    if (detector_)
        apriltag_detector_destroy(reinterpret_cast<apriltag_detector_t *>(detector_));
    if (family_)
        destroy_family(family_name_, reinterpret_cast<apriltag_family_t *>(family_));
}

std::vector<TagDetection> AprilTagCDecoder::detect(const cv::Mat &crop)
{
    CV_Assert(crop.type() == CV_8UC1);
    // No isContinuous() requirement: image_u8_t carries its own stride
    // (crop.step, below) and apriltag_detector_detect() indexes every pixel
    // through it (y*stride + x, throughout the library) -- a non-contiguous
    // sub-Mat view is correct input, not just tolerated. This is what lets
    // the caller pass a plain ROI view instead of cloning it first.

    image_u8_t im{crop.cols, crop.rows, static_cast<int32_t>(crop.step),
                  const_cast<uint8_t *>(crop.ptr<uint8_t>(0))};

    auto *det = reinterpret_cast<apriltag_detector_t *>(detector_);
    zarray_t *raw = apriltag_detector_detect(det, &im);

    std::vector<TagDetection> out;
    out.reserve(zarray_size(raw));
    for (int i = 0; i < zarray_size(raw); ++i)
    {
        apriltag_detection_t *d = nullptr;
        zarray_get(raw, i, &d);
        TagDetection t;
        t.id = d->id;
        t.hamming = d->hamming;
        t.decision_margin = d->decision_margin;
        t.center = cv::Point2f(static_cast<float>(d->c[0]), static_cast<float>(d->c[1]));
        for (int c = 0; c < 4; ++c)
            t.corners[c] = cv::Point2f(static_cast<float>(d->p[c][0]), static_cast<float>(d->p[c][1]));
        out.push_back(t);
    }
    apriltag_detections_destroy(raw);
    return out;
}

AprilTagRVVDecoder::AprilTagRVVDecoder(uint32_t min_blob_size, int mode) : handle_(nullptr), mode_(mode)
{
    handle_ = apriltag_new(min_blob_size);
    if (!handle_)
        throw std::runtime_error("AprilTagRVVDecoder: apriltag_new failed");
}

AprilTagRVVDecoder::~AprilTagRVVDecoder()
{
    if (handle_)
        apriltag_free(handle_);
}

std::vector<TagDetection> AprilTagRVVDecoder::detect(const cv::Mat &crop)
{
    CV_Assert(crop.type() == CV_8UC1);
    // Same zero-copy contract as AprilTagCDecoder::detect(): apriltag_detect()
    // takes an explicit stride, a non-contiguous cv::Mat view is correct input.
    constexpr int kMaxOut = 16; // generous for a single already-cropped ROI
    apriltag_det_t raw[kMaxOut];
    int n = apriltag_detect(handle_, crop.ptr<uint8_t>(0), static_cast<size_t>(crop.cols),
                             static_cast<size_t>(crop.rows), static_cast<size_t>(crop.step),
                             /*factor=*/0, mode_, raw, kMaxOut);

    std::vector<TagDetection> out;
    if (n < 0) // bad args / internal panic -- treat as "no detections", not a crash
        return out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        TagDetection t;
        t.id = static_cast<int>(raw[i].id);
        t.hamming = 0; // apriltag_det_t doesn't expose achieved hamming distance (see apriltag.h);
                        // 0 is accurate for "codeword matched", just not distinguishing how cleanly
        t.decision_margin = static_cast<float>(raw[i].margin);
        t.center = cv::Point2f(static_cast<float>(raw[i].center[0]), static_cast<float>(raw[i].center[1]));
        for (int c = 0; c < 4; ++c)
            t.corners[c] = cv::Point2f(static_cast<float>(raw[i].corners[2 * c]),
                                        static_cast<float>(raw[i].corners[2 * c + 1]));
        out.push_back(t);
    }
    return out;
}

std::shared_ptr<TagCropDecoder> make_crop_decoder()
{
    const char *env = std::getenv("TINYTAG_CV_DETECTOR");
    const std::string backend = env ? env : "aruco-nano";
    if (backend == "aruco-nano" || backend == "nano")
        return std::make_shared<ArucoNanoDecoder>(aruco_tolerant_mode());
    if (backend == "aruco2")
        return std::make_shared<Aruco2Decoder>(aruco_tolerant_mode());
    if (backend == "rvv")
        return std::make_shared<AprilTagRVVDecoder>();
    if (backend == "c")
        return std::make_shared<AprilTagCDecoder>();
    throw std::runtime_error(
        "TINYTAG_CV_DETECTOR must be 'aruco-nano', 'aruco2', 'rvv', or 'c'");
}
