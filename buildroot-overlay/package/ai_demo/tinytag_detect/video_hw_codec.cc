#include "video_hw_codec.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include "scoped_timing.hpp"
#include "tag_crop_decoder.h"
#include "tinytag_det.h"

using std::cerr;
using std::cout;
using std::endl;

namespace
{
struct HwEncoderState
{
    AVFormatContext *ofmt_ctx = nullptr;
    AVCodecContext *enc_ctx = nullptr;
    AVFrame *enc_frame = nullptr;
    AVPacket *out_pkt = nullptr;
    int out_stream_idx = -1;
    int64_t next_pts = 0;

    ~HwEncoderState()
    {
        if (enc_ctx)
        {
            avcodec_send_frame(enc_ctx, nullptr);
            while (out_pkt && ofmt_ctx && avcodec_receive_packet(enc_ctx, out_pkt) == 0)
            {
                av_packet_rescale_ts(out_pkt, enc_ctx->time_base, ofmt_ctx->streams[out_stream_idx]->time_base);
                out_pkt->stream_index = out_stream_idx;
                av_interleaved_write_frame(ofmt_ctx, out_pkt);
                av_packet_unref(out_pkt);
            }
        }
        if (ofmt_ctx && ofmt_ctx->pb)
            av_write_trailer(ofmt_ctx);
        if (enc_frame)
            av_frame_free(&enc_frame);
        if (out_pkt)
            av_packet_free(&out_pkt);
        if (enc_ctx)
            avcodec_free_context(&enc_ctx);
        if (ofmt_ctx)
        {
            if (ofmt_ctx->pb)
                avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);
        }
    }
};
} // namespace

int run_video_file_hw(const char *kmodel_path, const char *video_path, float heatmap_thres,
                       int max_proposals, float roi_expand, float roi_iou_thres, int debug_mode)
{
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened())
    {
        cerr << "hw codec: failed to open " << video_path << " (software decode)" << endl;
        return -1;
    }
    const int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (!(fps > 0.0))
        fps = 25.0; // matches run_video_file()'s software-path fallback
    AVRational time_base = av_d2q(1.0 / fps, 100000);

    HwEncoderState st;

    // Exact working parameter set from
    // buildroot-overlay/package/camera_rtsp_demo/src/media.cpp's
    // _init_encoder() (confirmed correct by that demo's real RTSP/WebRTC
    // streaming use) -- ffmpeg's own defaults for num_output_buffers/
    // num_capture_buffers don't work with this driver at all (QBUF I/O
    // errors), it needs exactly 1/1.
    const AVCodec *hw_encoder = avcodec_find_encoder_by_name("h264_v4l2m2m");
    if (!hw_encoder)
    {
        cerr << "hw codec: h264_v4l2m2m encoder not registered in this ffmpeg build" << endl;
        return -2;
    }
    st.enc_ctx = avcodec_alloc_context3(hw_encoder);
    st.enc_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    st.enc_ctx->codec_id = hw_encoder->id;
    st.enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    st.enc_ctx->width = width;
    st.enc_ctx->height = height;
    // YUV420P (I420), not NV12: negotiated fine with this driver
    // ("requesting formats: output=YU12 capture=H264") and lets the BGR
    // conversion go through cv::cvtColor(COLOR_BGR2YUV_I420) instead of
    // libswscale -- swapping that in cut the conversion step from ~64ms/
    // frame to a few ms (see experimental/README.md for the measurement
    // that found libswscale's BGR24->NV12 path was the actual bottleneck,
    // not the hardware codec itself).
    st.enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    st.enc_ctx->time_base = time_base;
    st.enc_ctx->bit_rate = 4000 * 1000; // evaluation/debug output, not a tuned streaming bitrate
    st.enc_ctx->gop_size = 30;
    st.enc_ctx->max_b_frames = 0;
    st.enc_ctx->rc_min_rate = st.enc_ctx->bit_rate;
    st.enc_ctx->rc_max_rate = st.enc_ctx->bit_rate;
    st.enc_ctx->rc_buffer_size = st.enc_ctx->bit_rate / 2;
    st.enc_ctx->rc_initial_buffer_occupancy = st.enc_ctx->rc_buffer_size * 3 / 4;

    AVDictionary *enc_opts = nullptr;
    av_dict_set_int(&enc_opts, "num_output_buffers", 1, 0);
    av_dict_set_int(&enc_opts, "num_capture_buffers", 1, 0);
    av_dict_set(&enc_opts, "tune", "zerolatency", 0);
    if (avcodec_open2(st.enc_ctx, hw_encoder, &enc_opts) < 0)
    {
        av_dict_free(&enc_opts);
        cerr << "hw codec: failed to open h264_v4l2m2m encoder" << endl;
        return -2;
    }
    av_dict_free(&enc_opts);

    const char *out_path = "tinytag_det.mp4";
    if (avformat_alloc_output_context2(&st.ofmt_ctx, nullptr, nullptr, out_path) < 0 || !st.ofmt_ctx)
    {
        cerr << "hw codec: failed to allocate output context for " << out_path << endl;
        return -1;
    }
    AVStream *out_stream = avformat_new_stream(st.ofmt_ctx, nullptr);
    st.out_stream_idx = out_stream->index;
    avcodec_parameters_from_context(out_stream->codecpar, st.enc_ctx);
    out_stream->time_base = st.enc_ctx->time_base;
    if (avio_open(&st.ofmt_ctx->pb, out_path, AVIO_FLAG_WRITE) < 0)
    {
        cerr << "hw codec: failed to open output file " << out_path << endl;
        return -1;
    }
    if (avformat_write_header(st.ofmt_ctx, nullptr) < 0)
    {
        cerr << "hw codec: failed to write output header" << endl;
        return -1;
    }

    st.enc_frame = av_frame_alloc();
    st.enc_frame->format = AV_PIX_FMT_YUV420P;
    st.enc_frame->width = width;
    st.enc_frame->height = height;
    // Alignment 1 (not the usual 32): makes linesize == width exactly (and
    // width/2 for the chroma planes), matching cv::cvtColor's tightly-packed
    // I420 output byte-for-byte so each plane can be memcpy'd directly below
    // with no row-by-row stride handling needed.
    if (av_frame_get_buffer(st.enc_frame, 1) < 0)
    {
        cerr << "hw codec: failed to allocate encoder frame buffer" << endl;
        return -1;
    }
    st.out_pkt = av_packet_alloc();

    cout << "hw codec: software decode -> TinyTagDet -> h264_v4l2m2m (hardware) encode, " << width << "x" << height
         << endl;

    auto decoder = make_crop_decoder(); // Default AprilTagRVVDecoder; TINYTAG_CV_DETECTOR=c selects AprilTagCDecoder
    TinyTagDet det(kmodel_path, heatmap_thres, max_proposals, roi_expand, roi_iou_thres, decoder, debug_mode);

    cv::Mat frame, gray, yuv_i420;
    int frame_idx = 0;
    int total_detections = 0;
    while (cap.read(frame))
    {
        ScopedTiming st_frame("frame " + std::to_string(frame_idx) + " total", debug_mode);
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

        std::vector<TinyTagResult> results;
        std::vector<Proposal> proposals;
        det.pre_process(gray);
        det.inference();
        det.post_process({static_cast<size_t>(width), static_cast<size_t>(height)}, gray, results, &proposals);

        if (debug_mode > 0)
            cout << "frame " << frame_idx << ": proposals=" << proposals.size() << " detections=" << results.size()
                 << endl;
        for (const auto &r : results)
        {
            if (debug_mode > 1)
                cout << "  id=" << r.id << " hamming=" << r.hamming << " margin=" << r.decision_margin
                     << " center=(" << r.center.x << "," << r.center.y << ")" << endl;
        }
        total_detections += static_cast<int>(results.size());

        det.draw_proposals(frame, proposals);
        det.draw_detections(frame, results);

        {
            ScopedTiming st_cvt("hw encode: BGR->I420", debug_mode);
            cv::cvtColor(frame, yuv_i420, cv::COLOR_BGR2YUV_I420);
            CV_Assert(yuv_i420.isContinuous());
            av_frame_make_writable(st.enc_frame);
            const uchar *src = yuv_i420.ptr(0);
            std::memcpy(st.enc_frame->data[0], src, static_cast<size_t>(width) * height);
            std::memcpy(st.enc_frame->data[1], src + static_cast<size_t>(width) * height,
                        static_cast<size_t>(width) * height / 4);
            std::memcpy(st.enc_frame->data[2], src + static_cast<size_t>(width) * height * 5 / 4,
                        static_cast<size_t>(width) * height / 4);
            st.enc_frame->pts = st.next_pts++;
        }
        {
            ScopedTiming st_send("hw encode: send_frame", debug_mode);
            if (avcodec_send_frame(st.enc_ctx, st.enc_frame) == 0)
            {
                ScopedTiming st_recv("hw encode: receive_packet+write", debug_mode);
                while (avcodec_receive_packet(st.enc_ctx, st.out_pkt) == 0)
                {
                    av_packet_rescale_ts(st.out_pkt, st.enc_ctx->time_base, out_stream->time_base);
                    st.out_pkt->stream_index = st.out_stream_idx;
                    av_interleaved_write_frame(st.ofmt_ctx, st.out_pkt);
                    av_packet_unref(st.out_pkt);
                }
            }
        }
        ++frame_idx;
    }

    cout << "wrote " << out_path << " (" << frame_idx << " frames, " << total_detections << " total detections)"
         << endl;
    return 0;
}
