#include "tag_crop_decoder.h"

#include <cstdlib>
#include <stdexcept>

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
    if (env && std::string(env) == "c")
        return std::make_shared<AprilTagCDecoder>();
    return std::make_shared<AprilTagRVVDecoder>();
}
