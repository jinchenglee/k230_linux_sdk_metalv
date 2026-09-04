#ifndef TAG_CROP_DECODER_H
#define TAG_CROP_DECODER_H

#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

// One decoded tag, in the coordinate space of whatever crop was passed to
// TagCropDecoder::detect() -- the caller offsets into full-frame coordinates.
struct TagDetection
{
    int id;
    int hamming;
    float decision_margin;
    cv::Point2f center;
    cv::Point2f corners[4]; // wind counter-clockwise, matches apriltag_detection_t::p
};

// decode_proposals() step 10 (see experimental/tinytag-v6/HEADS_AND_POSTPROCESSING.md):
// crop each neural-proposed ROI out of the full-resolution frame and hand it
// to a *traditional* CV tag decoder -- nothing neural past this point.
//
// TinyTagDet only ever talks to this interface, not to a specific detector
// implementation, on purpose: the plain AprilTag C library below is a
// correct-but-unoptimized default. A future backend (e.g. apriltag-rvv, or
// other optimized RVV post-processing kernels) can be dropped in without
// touching the neural proposal/decode_proposals code at all.
class TagCropDecoder
{
public:
    virtual ~TagCropDecoder() = default;

    // `crop` must be CV_8UC1 (grayscale). Does NOT need to be contiguous --
    // a plain sub-Mat view (e.g. full_res_gray(roi)) is fine and expected;
    // implementations should read `.step` as a proper stride rather than
    // assuming rows are packed. Returned detections are in crop's local
    // pixel coordinates.
    virtual std::vector<TagDetection> detect(const cv::Mat &crop) = 0;
};

// Default backend: the standard AprilRobotics apriltag C library already
// built into this SDK (buildroot-overlay/package/apriltag) -- the same
// library used by buildroot-overlay/package/apriltag_demo, used here
// unmodified via its public API (not the workload-instrumented ABI that
// apriltag_demo's own benchmarks use).
class AprilTagCDecoder : public TagCropDecoder
{
public:
    // Only "tag36h11" is wired up today (matches this SDK's other AprilTag
    // consumers); pass a different family name to get a clear error rather
    // than a silent wrong-family mismatch.
    //
    // Defaults otherwise match apriltag_demo: min_blob_size=25, exact
    // codewords, edge refinement off, and decode sharpening off.
    // quad_decimate default is 1.0 (no decimation), NOT the apriltag
    // library's own default of 2.0. That 2.0 default exists for full-frame
    // detection (tag is a small fraction of a large image, decimating the
    // whole frame before quad-finding is a real speed win); here `detect()`
    // only ever sees an already-cropped, already-upscaled (roi_expand)
    // proposal region, where a tag is a large fraction of a small image --
    // decimating it again throws away resolution on a target that's
    // already tight, for no clear benefit. Exposed as a constructor
    // parameter (not hardcoded) so it stays easy to sweep/tune later.
    explicit AprilTagCDecoder(const std::string &family_name = "tag36h11", float quad_decimate = 1.0f);
    ~AprilTagCDecoder() override;

    AprilTagCDecoder(const AprilTagCDecoder &) = delete;
    AprilTagCDecoder &operator=(const AprilTagCDecoder &) = delete;

    std::vector<TagDetection> detect(const cv::Mat &crop) override;

private:
    // apriltag_detector_t*/apriltag_family_t*, kept opaque so apriltag.h
    // doesn't leak into every translation unit that includes this header.
    void *detector_;
    void *family_;
    std::string family_name_;
};

// Alternative backend: the apriltag-rvv Rust crate (buildroot-overlay/
// package/apriltag_demo/lib/libapriltag_rvv.a), the same RVV-accelerated
// detector apriltag_demo.elf's --rvv mode uses. Its C ABI (apriltag_demo/
// src/apriltag.h) reports quad-search decimation coordinates already
// rescaled to the input crop's own resolution, so it drops in as a
// TagCropDecoder exactly like AprilTagCDecoder -- same offset math
// upstream in tinytag_det.cc, no special-casing needed there.
class AprilTagRVVDecoder : public TagCropDecoder
{
public:
    // min_blob_size: matches apriltag-rvv's own live_demo default (25).
    // mode: 0=scalar (the crate's own non-vectorized reimplementation),
    // 1=rvv (vectorized) -- defaults to rvv, since that's the point of
    // offering this as an alternative to AprilTagCDecoder.
    // quad-search decimation factor is fixed at 0 (=1.0, no decimation)
    // in detect() itself, matching AprilTagCDecoder's own default -- see
    // its constructor doc for why a cropped, already-upscaled ROI
    // shouldn't be decimated again.
    explicit AprilTagRVVDecoder(uint32_t min_blob_size = 25, int mode = 1);
    ~AprilTagRVVDecoder() override;

    AprilTagRVVDecoder(const AprilTagRVVDecoder &) = delete;
    AprilTagRVVDecoder &operator=(const AprilTagRVVDecoder &) = delete;

    std::vector<TagDetection> detect(const cv::Mat &crop) override;

private:
    void *handle_; // opaque apriltag-rvv detector handle (apriltag_new/apriltag_free)
    int mode_;
};

// Picks a crop-decode backend based on the TINYTAG_CV_DETECTOR environment
// variable: unset/"rvv" -> AprilTagRVVDecoder (production default),
// "c" -> AprilTagCDecoder (reference fallback). All call sites (live camera,
// video-file,
// image mode) should construct their decoder through this instead of
// naming a concrete class directly, so they can't drift out of sync with
// each other.
std::shared_ptr<TagCropDecoder> make_crop_decoder();

#endif
