#include "apriltag.h"
#include "apriltag_draw.h"

#include <cassert>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>

static_assert(sizeof(apriltag_det_t) == 120);
static_assert(offsetof(apriltag_det_t, recovered) == 96);
static_assert(offsetof(apriltag_det_t, geometry_residual) == 104);
static_assert(offsetof(apriltag_det_t, recovery_group) == 112);
static_assert(sizeof(apriltag_recovery_stats_t) == 112);
static_assert(sizeof(apriltag_recovery_candidate_t) == 40);

// Buildroot compiles this with -DNDEBUG -O2, so assert() bodies and unused
// local function pointers disappear before the linker runs. An externally
// visible table keeps relocations against the recovery ABI, which forces the
// linker to pull those symbols out of libapriltag_rvv.a and makes the
// POST_BUILD symbol check in CMakeLists.txt meaningful.
extern void* const apriltag_recovery_abi[3];
void* const apriltag_recovery_abi[3] = {
    reinterpret_cast<void*>(&apriltag_configure_recovery),
    reinterpret_cast<void*>(&apriltag_get_recovery_stats),
    reinterpret_cast<void*>(&apriltag_get_recovery_candidates),
};

int main()
{
    assert(apriltag_debug_stage_name(6) == std::string("recovery"));

    cv::Mat osd(120, 160, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    apriltag_det_t detection = {};
    detection.id = 7;
    detection.recovered = 1;
    detection.center[0] = 80;
    detection.center[1] = 60;
    const double corners[] = {30, 30, 130, 30, 130, 90, 30, 90};
    std::copy(std::begin(corners), std::end(corners), detection.corners);
    draw_detections(osd, {detection}, 160, 120);

    const cv::Vec4b magenta = osd.at<cv::Vec4b>(30, 80);
    assert(magenta[0] == 255 && magenta[1] == 0 && magenta[2] == 255);

    apriltag_recovery_stats_t stats = {};
    stats.group_d_total = 3;
    stats.group_d_eligible = 1;
    stats.trials_attempted = 1;
    draw_recovery_stats(osd, stats);

    apriltag_recovery_candidate_t candidate = {};
    candidate.group = 2;
    candidate.strong_line_count = 3;
    candidate.eligible = 1;
    candidate.selected = 1;
    candidate.result = 2;
    candidate.id = 7;
    candidate.errors = 1;
    candidate.erasures = 2;
    candidate.anchor[0] = 80;
    candidate.anchor[1] = 60;
    draw_recovery_candidates(osd, 160, 120, &candidate, 1);
    return 0;
}
