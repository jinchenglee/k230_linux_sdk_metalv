#include "tinytag_det.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// Native 16:9 crop target for the network band (matches the kmodel's
// 640x360 at 2x). Inputs larger than this are cropped to the BOTTOM-LEFT
// 1280x720 window (throw away any top rows / right columns beyond it);
// inputs smaller than or equal to it are used as-is (no scale-up / pad --
// see README). Kept a plain constant so pre_process() and
// decode_proposals() stay consistent about which region the NN saw.
constexpr int kCropW = 1280;
constexpr int kCropH = 720;

TinyTagDet::TinyTagDet(const char *kmodel_file, float heatmap_thres, int max_proposals, float roi_expand,
                       float roi_iou_thres, std::shared_ptr<TagCropDecoder> decoder, const int debug_mode)
    : AIBase(kmodel_file, "TinyTagDet", debug_mode),
      heatmap_thres_(heatmap_thres), max_proposals_(max_proposals), roi_expand_(roi_expand),
      roi_iou_thres_(roi_iou_thres), decoder_(std::move(decoder))
{
    model_name_ = "TinyTagDet";
    ai2d_out_tensor_ = this->get_input_tensor(0);
}

TinyTagDet::~TinyTagDet() {}

void TinyTagDet::pre_process(cv::Mat ori_img_gray)
{
    ScopedTiming st(model_name_ + " pre_process", debug_mode_);
    CV_Assert(ori_img_gray.type() == CV_8UC1);

    // Crop to the network's native 16:9 band, keeping the BOTTOM-LEFT
    // 1280x720 window (discard any top rows / right columns beyond it).
    // No-op for sources already <= 1280x720. This is a zero-cost sub-Mat
    // view (no resize, no copy) handed straight to the ai2d hardware
    // downscale, so the network sees an undistorted 16:9 image instead of
    // a non-uniform stretch (previously a 1280x800 frame was resized to
    // 640x360, squishing the scene vertically). Crop-only by design -- no
    // scale-up/pad for inputs smaller than 1280x720.
    const int src_w = ori_img_gray.cols;
    const int src_h = ori_img_gray.rows;
    const int crop_w = std::min(src_w, kCropW);
    const int crop_h = std::min(src_h, kCropH);
    const int crop_x = 0;                  // keep LEFT columns
    const int crop_y = src_h - crop_h;     // keep BOTTOM rows
    cv::Mat gray = ori_img_gray(cv::Rect(crop_x, crop_y, crop_w, crop_h));

    const int w = gray.cols;
    const int h = gray.rows;

    if (!ai2d_builder_ || w != ai2d_in_w_ || h != ai2d_in_h_)
    {
        // (Re)build only when the input shape actually changes -- happens
        // at most once per run for a fixed-resolution source (camera, or a
        // single video file/image), not every frame. build_schedule() is
        // real one-time setup cost (see experimental/README.md); previously
        // this ran on every single frame.
        ScopedTiming st_build(model_name_ + " pre_process: ai2d_builder rebuild (shape change)", debug_mode_);
        dims_t in_shape{1, 1, static_cast<size_t>(h), static_cast<size_t>(w)};
        ai2d_in_tensor_ =
            host_runtime_tensor::create(typecode_t::dt_uint8, in_shape, hrt::pool_shared).expect("cannot create ai2d input tensor");
        Utils::resize({1, static_cast<size_t>(h), static_cast<size_t>(w)}, ai2d_builder_, ai2d_in_tensor_,
                       ai2d_out_tensor_, false);
        ai2d_in_w_ = w;
        ai2d_in_h_ = h;
    }

    // Write straight into the persistent input tensor's own mapped buffer
    // -- no intermediate chw_vec, no per-frame tensor allocation. Still a
    // row-by-row copy (not a single memcpy): do NOT assume
    // ori_img_gray.isContinuous(), since a sub-Mat view (e.g. a cropped
    // camera frame, or anything upstream that pads stride) is valid input
    // here too and copying by row respects .step regardless.
    {
        ScopedTiming st_copy(model_name_ + " pre_process: memcpy+sync", debug_mode_);
        auto input_buf =
            ai2d_in_tensor_.impl()->to_host().unwrap()->buffer().as_host().unwrap().map(map_access_::map_write).unwrap().buffer();
        uint8_t *dst = reinterpret_cast<uint8_t *>(input_buf.data());
        for (int y = 0; y < h; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * w, gray.ptr<uint8_t>(y), w);
        hrt::sync(ai2d_in_tensor_, sync_op_t::sync_write_back, true).expect("write back input failed");
    }

    {
        ScopedTiming st_invoke(model_name_ + " pre_process: ai2d invoke", debug_mode_);
        ai2d_builder_->invoke(ai2d_in_tensor_, ai2d_out_tensor_).expect("error occurred in ai2d running");
    }
}

void TinyTagDet::inference()
{
    // run() and get_output() are each individually ScopedTiming'd inside
    // AIBase -- this is the "Conv forward only" line of
    // HEADS_AND_POSTPROCESSING.md's timing table, on real K230 hardware
    // instead of GPU.
    this->run();
    this->get_output();
}

void TinyTagDet::decode_proposals(FrameSize frame_size, std::vector<Proposal> &proposals)
{
    // p_outputs_[0]: NCHW float32 [1, 21, H, W]. Only channels 0-4 are
    // trained (heatmap, offset_x, offset_y, scale_w, scale_h) -- see
    // experimental/tinytag-v6/HEADS_AND_POSTPROCESSING.md. Channels 5-20
    // (corner/visibility) are dormant and deliberately unread here.
    const int H = output_shapes_[0][2];
    const int W = output_shapes_[0][3];
    const int plane = H * W;
    const float *out = p_outputs_[0];
    const float *heatmap = out + 0 * plane;
    const float *offset_x = out + 1 * plane;
    const float *offset_y = out + 2 * plane;
    const float *scale_w = out + 3 * plane;
    const float *scale_h = out + 4 * plane;

    std::vector<float> score(plane);
    std::vector<uint8_t> is_peak(plane, 0);
    {
        ScopedTiming st(model_name_ + " post_process: sigmoid+nms", debug_mode_);
        for (int i = 0; i < plane; ++i)
            score[i] = 1.f / (1.f + std::exp(-heatmap[i]));

        // 3x3 max-pool local-maxima NMS: a cell survives only if it equals
        // the max of its own 3x3 neighborhood (clamped at the border).
        // Reads `score` only -- never mutated mid-scan, so neighbor reads
        // stay correct regardless of scan order.
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                float v = score[y * W + x];
                bool peak = true;
                for (int dy = -1; dy <= 1 && peak; ++dy)
                {
                    int ny = y + dy;
                    if (ny < 0 || ny >= H)
                        continue;
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        int nx = x + dx;
                        if (nx < 0 || nx >= W)
                            continue;
                        if (score[ny * W + nx] > v)
                        {
                            peak = false;
                            break;
                        }
                    }
                }
                is_peak[y * W + x] = peak ? 1 : 0;
            }
        }
    }

    std::vector<std::pair<float, int>> peaks;
    {
        ScopedTiming st(model_name_ + " post_process: threshold+topk", debug_mode_);
        for (int i = 0; i < plane; ++i)
            if (is_peak[i] && score[i] >= heatmap_thres_)
                peaks.emplace_back(score[i], i);
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        if (static_cast<int>(peaks.size()) > max_proposals_)
            peaks.resize(max_proposals_);
    }

    {
        ScopedTiming st(model_name_ + " post_process: decode+roi_expand+clamp", debug_mode_);
        // The network was fed the bottom-left 16:9 band (see pre_process),
        // so map network coords into that band's space, then offset back to
        // full-frame coords. For sources <= 1280x720 this reduces to the
        // previous behavior (crop == whole frame, offsets == 0).
        const int band_w = std::min(static_cast<int>(frame_size.width), kCropW);
        const int band_h = std::min(static_cast<int>(frame_size.height), kCropH);
        const int crop_x = 0;
        const int crop_y = static_cast<int>(frame_size.height) - band_h;
        const float x_factor = static_cast<float>(band_w) / input_shapes_[0][3];
        const float y_factor = static_cast<float>(band_h) / input_shapes_[0][2];

        proposals.reserve(peaks.size());
        for (auto &pk : peaks)
        {
            int idx = pk.second;
            int gy = idx / W;
            int gx = idx % W;

            float cx = (gx + offset_x[idx]) * kStride;
            float cy = (gy + offset_y[idx]) * kStride;
            float w = std::exp(std::clamp(scale_w[idx], kScaleClampLo, kScaleClampHi)) * kStride;
            float h = std::exp(std::clamp(scale_h[idx], kScaleClampLo, kScaleClampHi)) * kStride;
            w *= roi_expand_;
            h *= roi_expand_;

            cx *= x_factor;
            cy *= y_factor;
            w *= x_factor;
            h *= y_factor;

            // Offset band coords into full-frame coords (network saw the
            // bottom-left band), and clamp the decoded box to the kept band
            // so the CV crop-decode never reads cropped-away top/right rows.
            cx += crop_x;
            cy += crop_y;
            float x0 = std::clamp(cx - w / 2.f, static_cast<float>(crop_x), static_cast<float>(crop_x + band_w));
            float y0 = std::clamp(cy - h / 2.f, static_cast<float>(crop_y), static_cast<float>(crop_y + band_h));
            float x1 = std::clamp(cx + w / 2.f, static_cast<float>(crop_x), static_cast<float>(crop_x + band_w));
            float y1 = std::clamp(cy + h / 2.f, static_cast<float>(crop_y), static_cast<float>(crop_y + band_h));
            if (x1 <= x0 || y1 <= y0)
                continue;

            proposals.push_back({pk.first, cv::Rect2f(x0, y0, x1 - x0, y1 - y0)});
        }
    }

    // Box-level IoU suppression. `proposals` is already in descending
    // confidence order here (peaks were sorted by score before the top-K
    // cut, and pushed in that order), so a plain greedy keep-first pass is
    // correct without re-sorting. O(n^2) over at most max_proposals_ (20)
    // entries -- negligible next to the ~7-8ms-per-ROI CV decode this
    // exists to avoid. See the constructor doc for why this isn't in the
    // reference spec.
    if (roi_iou_thres_ > 0.f && proposals.size() > 1)
    {
        ScopedTiming st(model_name_ + " post_process: roi_iou_suppress", debug_mode_);
        std::vector<Proposal> kept;
        kept.reserve(proposals.size());
        for (const auto &cand : proposals)
        {
            bool suppressed = false;
            for (const auto &k : kept)
            {
                if (rect_iou(cand.roi, k.roi) > roi_iou_thres_)
                {
                    suppressed = true;
                    break;
                }
            }
            if (!suppressed)
                kept.push_back(cand);
        }
        if (debug_mode_ > 1)
        {
            char line[128];
            std::snprintf(line, sizeof(line), "roi_iou_suppress: %zu -> %zu proposals\n",
                          proposals.size(), kept.size());
            scoped_timing_write(line);
        }
        proposals.swap(kept);
    }
}

float TinyTagDet::rect_iou(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const float inter = (a & b).area();
    const float uni = a.area() + b.area() - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

void TinyTagDet::post_process(FrameSize frame_size, cv::Mat full_res_gray,
                               std::vector<TinyTagResult> &results,
                               std::vector<Proposal> *all_proposals)
{
    ScopedTiming st_total(model_name_ + " post_process: total", debug_mode_);
    CV_Assert(full_res_gray.type() == CV_8UC1);

    std::vector<Proposal> proposals;
    decode_proposals(frame_size, proposals);
    if (all_proposals)
        *all_proposals = proposals;

    std::vector<TinyTagResult> raw;
    {
        ScopedTiming st(model_name_ + " post_process: cv_crop_decode (all rois, n=" +
                             std::to_string(proposals.size()) + ")",
                         debug_mode_);
        for (auto &p : proposals)
        {
            cv::Rect roi_i(static_cast<int>(std::lround(p.roi.x)), static_cast<int>(std::lround(p.roi.y)),
                           static_cast<int>(std::lround(p.roi.width)),
                           static_cast<int>(std::lround(p.roi.height)));
            roi_i &= cv::Rect(0, 0, full_res_gray.cols, full_res_gray.rows);
            if (roi_i.width <= 0 || roi_i.height <= 0)
                continue;

            std::vector<TagDetection> dets;
            {
                // Per-ROI timing only at debug_mode 2+ (matches the SDK
                // convention: 1 = per-stage timing, 2 = + verbose) so
                // enabling profiling doesn't spam N lines per frame by
                // default -- the "all rois" scope above already gives the
                // aggregate cost. Logs dets.size() alongside the time
                // (not just the time alone, like a plain ScopedTiming would)
                // -- correlating cost with hit/miss is what tells apart "this
                // backend does the same search faster" from "this backend
                // gives up earlier on hard ROIs, which is why it's faster
                // AND finds less" -- see experimental/README.md's
                // TINYTAG_CV_DETECTOR backend comparison.
                auto t0 = std::chrono::steady_clock::now();
                // Zero-copy: a plain sub-Mat view, no .clone(). AprilTagCDecoder::detect()
                // (and the underlying image_u8_t stride field) handle a non-contiguous
                // view correctly -- see tag_crop_decoder.h/.cc.
                cv::Mat crop = full_res_gray(roi_i);
                dets = decoder_->detect(crop);
                if (debug_mode_ > 1)
                {
                    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    char line[160];
                    std::snprintf(line, sizeof(line),
                                  "post_process: cv_crop_decode (one roi) dets=%zu took %.4f ms\n",
                                  dets.size(), ms);
                    scoped_timing_write(line);
                }
            }
            for (auto &d : dets)
            {
                TinyTagResult r;
                r.id = d.id;
                r.hamming = d.hamming;
                r.decision_margin = d.decision_margin;
                r.proposal_confidence = p.confidence;
                r.roi = cv::Rect2f(static_cast<float>(roi_i.x), static_cast<float>(roi_i.y),
                                    static_cast<float>(roi_i.width), static_cast<float>(roi_i.height));
                cv::Point2f off(static_cast<float>(roi_i.x), static_cast<float>(roi_i.y));
                r.center = d.center + off;
                for (int c = 0; c < 4; ++c)
                    r.corners[c] = d.corners[c] + off;
                raw.push_back(r);
            }
        }
    }

    {
        ScopedTiming st(model_name_ + " post_process: dedupe", debug_mode_);
        dedupe(raw, results);
    }
}

void TinyTagDet::dedupe(const std::vector<TinyTagResult> &raw, std::vector<TinyTagResult> &out)
{
    // O(n^2) -- fine, n is a handful of tags per frame (matches the
    // training repo's own dedupe(), which is also a plain double loop for
    // the same reason). Keeps the higher decision_margin of each
    // same-ID, close-center pair; HEADS_AND_POSTPROCESSING.md doesn't say
    // which one the reference implementation keeps, so this tie-break is
    // this port's own reasonable choice, not a documented requirement.
    std::vector<uint8_t> dropped(raw.size(), 0);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (dropped[i])
            continue;
        for (size_t j = i + 1; j < raw.size(); ++j)
        {
            if (dropped[j] || raw[j].id != raw[i].id)
                continue;
            float dist = cv::norm(raw[i].center - raw[j].center);
            if (dist <= kDedupeDistPx)
                dropped[raw[j].decision_margin > raw[i].decision_margin ? i : j] = 1;
        }
    }
    out.clear();
    for (size_t i = 0; i < raw.size(); ++i)
        if (!dropped[i])
            out.push_back(raw[i]);
}

void TinyTagDet::draw_detections(cv::Mat &frame, const std::vector<TinyTagResult> &results)
{
    for (const auto &r : results)
    {
        cv::rectangle(frame, r.roi, cv::Scalar(0, 165, 255), 1);
        for (int c = 0; c < 4; ++c)
            cv::line(frame, r.corners[c], r.corners[(c + 1) % 4], cv::Scalar(0, 255, 0), 2);
        cv::circle(frame, r.center, 3, cv::Scalar(0, 0, 255), cv::FILLED);
        std::string label = "id=" + std::to_string(r.id);
        cv::putText(frame, label, r.center + cv::Point2f(6, -6), cv::FONT_HERSHEY_DUPLEX, 0.6,
                    cv::Scalar(0, 255, 0), 1);
    }
}

void TinyTagDet::draw_detections_scaled(cv::Mat &frame, const std::vector<TinyTagResult> &results,
                                         size_t src_w, size_t src_h)
{
    const float sx = static_cast<float>(frame.cols) / static_cast<float>(src_w);
    const float sy = static_cast<float>(frame.rows) / static_cast<float>(src_h);
    for (const auto &r : results)
    {
        cv::Rect2f roi(r.roi.x * sx, r.roi.y * sy, r.roi.width * sx, r.roi.height * sy);
        cv::Point2f center(r.center.x * sx, r.center.y * sy);
        cv::Point2f corners[4];
        for (int c = 0; c < 4; ++c)
            corners[c] = cv::Point2f(r.corners[c].x * sx, r.corners[c].y * sy);

        cv::rectangle(frame, roi, cv::Scalar(0, 165, 255, 255), 1);
        for (int c = 0; c < 4; ++c)
            cv::line(frame, corners[c], corners[(c + 1) % 4], cv::Scalar(0, 255, 0, 255), 2);
        cv::circle(frame, center, 3, cv::Scalar(0, 0, 255, 255), cv::FILLED);
        std::string label = "id=" + std::to_string(r.id);
        cv::putText(frame, label, center + cv::Point2f(6, -6), cv::FONT_HERSHEY_DUPLEX, 0.6,
                    cv::Scalar(0, 255, 0, 255), 1);
    }
}

void TinyTagDet::draw_proposals(cv::Mat &frame, const std::vector<Proposal> &proposals)
{
    for (const auto &p : proposals)
    {
        // Cyan, thin, dashed-ish (drawn under draw_detections' green quads
        // when both are called) -- distinguishes "neural candidate, not yet
        // CV-confirmed" from an actually decoded tag.
        cv::rectangle(frame, p.roi, cv::Scalar(255, 255, 0), 1);
        char label[16];
        std::snprintf(label, sizeof(label), "p=%.2f", p.confidence);
        cv::putText(frame, label, cv::Point2f(p.roi.x, p.roi.y - 4), cv::FONT_HERSHEY_DUPLEX, 0.45,
                    cv::Scalar(255, 255, 0), 1);
    }
}

void TinyTagDet::draw_proposals_scaled(cv::Mat &frame, const std::vector<Proposal> &proposals, size_t src_w,
                                        size_t src_h)
{
    const float sx = static_cast<float>(frame.cols) / static_cast<float>(src_w);
    const float sy = static_cast<float>(frame.rows) / static_cast<float>(src_h);
    for (const auto &p : proposals)
    {
        cv::Rect2f roi(p.roi.x * sx, p.roi.y * sy, p.roi.width * sx, p.roi.height * sy);
        cv::rectangle(frame, roi, cv::Scalar(255, 255, 0, 255), 1);
        char label[16];
        std::snprintf(label, sizeof(label), "p=%.2f", p.confidence);
        cv::putText(frame, label, cv::Point2f(roi.x, roi.y - 4), cv::FONT_HERSHEY_DUPLEX, 0.45,
                    cv::Scalar(255, 255, 0, 255), 1);
    }
}
