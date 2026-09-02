#ifndef TINYTAG_DET_H
#define TINYTAG_DET_H

#include "ai_base.h"
#include "tag_crop_decoder.h"
#include "utils.h"

#include <opencv2/core.hpp>
#include <memory>
#include <vector>

// One neural proposal, before CV crop-decode -- exposed separately from
// TinyTagResult so the neural side can be profiled/debugged in isolation
// (decode_proposals() vs. the CV crop-decode stage are very different
// costs on K230; see experimental/tinytag-v6/HEADS_AND_POSTPROCESSING.md
// for the GPU-side timing breakdown this is meant to be compared against).
struct Proposal
{
    float confidence; // sigmoid(heatmap) at the peak cell
    cv::Rect2f roi;    // full-frame pixel coords, after roi_expand + clamp
};

// Detector-owned copy of one clipped proposal ROI. Live capture creates
// these before returning its V4L2 buffer to the driver.
struct ProposalCrop
{
    Proposal proposal;
    cv::Rect roi;
    cv::Mat gray;
};

// One decoded tag, in full-frame pixel coordinates.
struct TinyTagResult
{
    int id;
    int hamming;
    float decision_margin;
    float proposal_confidence; // the neural proposal that produced this ROI
    cv::Rect2f roi;
    cv::Point2f center;
    cv::Point2f corners[4];
};

// TinyTag neural-proposal + AprilTag crop-decode pipeline. See
// experimental/tinytag-v6/HEADS_AND_POSTPROCESSING.md for the full spec
// this is a direct C++ port of (decode_proposals() steps 1-8 here in
// decode_proposals(), steps 9-11 in post_process()); no reference Python
// source for it exists in this repo.
class TinyTagDet : public AIBase
{
public:
    // heatmap_thres: keep cells with sigmoid(heatmap) >= this (0.20 at the
    //   training repo's frozen operating point).
    // max_proposals: top-K cap on proposals per frame (20 at that point).
    // roi_expand: safety margin multiplied onto each proposal's decoded
    //   size before cropping (1.5 at the training repo's frozen operating
    //   point) -- see decode_proposals() step 7.
    // decoder: CV crop-decode backend (see tag_crop_decoder.h) -- pass a
    //   different implementation to swap the traditional-CV stage without
    //   touching this class.
    // roi_iou_thres: box-level IoU suppression applied to the decoded ROIs
    //   before CV crop-decode -- drop a lower-confidence proposal whose box
    //   overlaps a kept higher-confidence one by more than this. <= 0
    //   disables it. NOT part of decode_proposals()'s reference spec (see
    //   HEADS_AND_POSTPROCESSING.md): that spec's only NMS is the 3x3
    //   max-pool over *heatmap grid cells* (step 2), which suppresses
    //   non-maximal cells but says nothing about the boxes those cells
    //   decode into -- two peaks >=2 cells apart both survive it, yet after
    //   exp() size decode and roi_expand their boxes can overlap almost
    //   entirely. Each surviving ROI costs a full AprilTag detect() call
    //   (~7-8ms on K230), so this is a deliberate deviation for cost, not a
    //   port of upstream behavior. Disable it to match the spec exactly.
    TinyTagDet(const char *kmodel_file, float heatmap_thres, int max_proposals, float roi_expand,
               float roi_iou_thres, std::shared_ptr<TagCropDecoder> decoder, const int debug_mode = 1);
    ~TinyTagDet();

    // ori_img_gray must be CV_8UC1. For inputs larger than 1280x720, it is
    // first cropped (zero-copy sub-Mat view) to the BOTTOM-LEFT 1280x720
    // band -- throwing away any top rows / right columns -- so the network
    // sees an undistorted native 16:9 image instead of a non-uniform stretch.
    // Inputs <= 1280x720 are used as-is (crop-only by design; no scale-up /
    // letterbox-pad -- see README). Then resized (no letterbox/pad -- matches
    // training preprocessing per HEADS_AND_POSTPROCESSING.md's fixed x2
    // scale factor implying a plain resize, not aspect-preserving) into the
    // kmodel's input tensor.
    void pre_process(cv::Mat ori_img_gray);

    void inference();

    // decode_proposals() steps 1-8: sigmoid, 3x3-max-pool NMS, threshold,
    // top-K, center/size decode, roi_expand, clamp. frame_size is the size
    // of the image passed to pre_process(); proposal ROIs come back scaled
    // into that space. Because pre_process() crops the network input to the
    // bottom-left 1280x720 band, this maps network coords into that band and
    // then offsets back to full-frame coords (generalizes the training doc's
    // fixed "x2 to full resolution" into band_size/network_input_shape).
    void decode_proposals(FrameSize frame_size, std::vector<Proposal> &proposals);

    // Copy each proposal ROI while full_res_gray is valid. The source may be
    // a V4L2 mmap and is not accessed after this call returns.
    static void copy_proposal_crops(cv::Mat full_res_gray,
                                    const std::vector<Proposal> &proposals,
                                    std::vector<ProposalCrop> &crops,
                                    size_t &crop_count);

    // CPU-heavy crop decode from detector-owned ROI copies. Kept separate so
    // live capture can release its V4L2 buffer before this work begins.
    void decode_proposal_crops(const std::vector<ProposalCrop> &crops,
                               size_t crop_count,
                               std::vector<TinyTagResult> &results);

    // Full pipeline: decode_proposals() then, per proposal, crop
    // full_res_gray and run it through `decoder_` (steps 9-11: crop-decode
    // + dedupe). full_res_gray must be CV_8UC1 and the same size as
    // frame_size passed here (both describe "the frame proposals were
    // decoded into", kept separate only because decode_proposals() doesn't
    // need pixel data). If all_proposals is non-null, it's filled with
    // every proposal that survived decode_proposals() (i.e. entered CV
    // crop-decode) -- including ones the CV decoder didn't confirm as a
    // real tag -- without re-running the neural side a second time.
    void post_process(FrameSize frame_size, cv::Mat full_res_gray, std::vector<TinyTagResult> &results,
                       std::vector<Proposal> *all_proposals = nullptr);

    void draw_detections(cv::Mat &frame, const std::vector<TinyTagResult> &results);

    // Draws every surviving proposal ROI (not just CV-confirmed ones) with
    // its neural confidence, so a proposal the CV decoder rejected is still
    // visible on screen -- useful for judging whether the neural side or
    // the CV decode side is the one failing on a given frame.
    void draw_proposals(cv::Mat &frame, const std::vector<Proposal> &proposals);

    // Like draw_detections, but for the video-mode OSD overlay: `frame` is
    // sized to the *display's* buffer, not the sensor's, so results (in
    // src_w x src_h sensor-pixel space) need rescaling into it -- mirrors
    // OBDet::draw_result_video's rect_x/rect_y/rect_w/rect_h scaling.
    static void draw_detections_scaled(cv::Mat &frame, const std::vector<TinyTagResult> &results,
                                        size_t src_w, size_t src_h);

    // draw_proposals's video-mode-OSD counterpart, same scaling as
    // draw_detections_scaled.
    static void draw_proposals_scaled(cv::Mat &frame, const std::vector<Proposal> &proposals, size_t src_w,
                                       size_t src_h);

private:
    void dedupe(const std::vector<TinyTagResult> &raw, std::vector<TinyTagResult> &out);

    static float rect_iou(const cv::Rect2f &a, const cv::Rect2f &b);

    float heatmap_thres_;
    int max_proposals_;
    float roi_expand_;
    float roi_iou_thres_;
    std::shared_ptr<TagCropDecoder> decoder_;

    static constexpr int kStride = 8;
    static constexpr float kScaleClampLo = -4.0f;
    static constexpr float kScaleClampHi = 6.0f;
    // Not specified precisely in HEADS_AND_POSTPROCESSING.md ("within a
    // small pixel distance") -- a reasonable starting default, not a value
    // taken from the training repo. Retune against real K230 footage.
    static constexpr float kDedupeDistPx = 20.0f;

    runtime_tensor ai2d_out_tensor_;

    // Persistent ai2d resize pipeline: built once per distinct input shape
    // (lazily, on first pre_process() call, or again if the shape changes)
    // rather than every frame -- build_schedule() is real setup cost, and
    // the input shape is constant for the whole run of a given source
    // (fixed camera resolution, or one video file), so rebuilding it per
    // frame was pure waste. Matches the pattern already used elsewhere in
    // this SDK (e.g. face_detection.h's ai2d_builder_/ai2d_in_tensor_).
    std::unique_ptr<ai2d_builder> ai2d_builder_;
    runtime_tensor ai2d_in_tensor_;
    int ai2d_in_w_ = -1;
    int ai2d_in_h_ = -1;
};
#endif
