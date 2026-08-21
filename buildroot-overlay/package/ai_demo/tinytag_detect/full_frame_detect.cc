// Standalone ground-truth-quality probe: runs a TagCropDecoder backend
// (TINYTAG_CV_DETECTOR's "c"/"rvv" choice, reused directly -- see
// tag_crop_decoder.h) on the FULL frame (not a neural-proposed ROI crop),
// optionally upscaled first, to see whether a stronger full-frame detector
// config recovers more real tags than the plain reference-C-at-1x baseline
// used so far for bos_logs_video ground truth (see experimental/README.md).
// Not part of the shipped app -- a debug/evaluation tool, cross-compiled
// standalone like experimental/tools/kdump.
#include "tag_crop_decoder.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: full_frame_detect <video> <backend c|rvv> <scale e.g. 1.0|2.0> [max_frames] [out_json]" << std::endl;
        return 1;
    }
    const char *video_path = argv[1];
    const char *backend = argv[2];
    double scale = std::atof(argv[3]);
    int max_frames = argc > 4 ? std::atoi(argv[4]) : -1;
    const char *out_json = argc > 5 ? argv[5] : nullptr;
    std::ofstream jf;
    if (out_json)
    {
        jf.open(out_json);
        jf << "{\"per_frame\":[";
    }

    setenv("TINYTAG_CV_DETECTOR", backend, 1);
    auto decoder = make_crop_decoder();

    cv::VideoCapture cap(video_path);
    if (!cap.isOpened())
    {
        std::cerr << "cannot open " << video_path << std::endl;
        return 1;
    }

    cv::Mat frame, gray, scaled;
    int frame_idx = 0;
    long long total_dets = 0;
    double total_ms = 0.0;
    while (cap.read(frame))
    {
        if (max_frames >= 0 && frame_idx >= max_frames)
            break;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        const cv::Mat *target = &gray;
        if (scale != 1.0)
        {
            cv::resize(gray, scaled, cv::Size(), scale, scale, cv::INTER_LINEAR);
            target = &scaled;
        }

        auto t0 = std::chrono::steady_clock::now();
        auto dets = decoder->detect(*target);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total_ms += ms;
        total_dets += static_cast<long long>(dets.size());

        if (out_json)
        {
            if (frame_idx > 0)
                jf << ",";
            jf << "{\"frame\":" << frame_idx << ",\"n\":" << dets.size() << ",\"ids\":[";
            for (size_t i = 0; i < dets.size(); ++i)
            {
                if (i)
                    jf << ",";
                jf << dets[i].id;
            }
            jf << "]}";
        }

        ++frame_idx;
        if (frame_idx % 100 == 0)
            std::cerr << "  " << frame_idx << " frames, " << total_dets << " detections so far..." << std::endl;
    }

    if (out_json)
    {
        jf << "],\"backend\":\"" << backend << "\",\"scale\":" << scale
           << ",\"frames\":" << frame_idx << ",\"total_detections\":" << total_dets << "}";
        jf.close();
    }

    std::cout << "backend=" << backend << " scale=" << scale << " frames=" << frame_idx
              << " total_detections=" << total_dets << " total_time_ms=" << total_ms
              << " avg_ms_per_frame=" << (frame_idx ? total_ms / frame_idx : 0.0) << std::endl;
    return 0;
}
