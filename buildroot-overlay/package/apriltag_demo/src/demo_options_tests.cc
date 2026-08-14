#include "demo_options.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message);

std::vector<std::string> calls;
uint32_t setter_mode = UINT32_MAX;
bool fail_new = false;
bool fail_set = false;

void* fake_new(uint32_t min_blob)
{
    calls.push_back("new:" + std::to_string(min_blob));
    return fail_new ? nullptr : reinterpret_cast<void*>(0x1234);
}

int fake_set(apriltag_t* detector, uint32_t mode)
{
    expect(detector == reinterpret_cast<void*>(0x1234),
           "setter receives constructed detector");
    calls.push_back("set");
    setter_mode = mode;
    return fail_set ? -1 : 0;
}

void fake_free(void* detector)
{
    expect(detector == reinterpret_cast<void*>(0x1234),
           "free receives constructed detector");
    calls.push_back("free");
}

void reset_fakes()
{
    calls.clear();
    setter_mode = UINT32_MAX;
    fail_new = false;
    fail_set = false;
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    bool local = false;
    std::string error;

    expect(parse_ccl_scratch_option("--factor", false, local, error) == 0,
           "unrelated option is not handled");
    expect(!local && error.empty(), "unrelated option does not change state");

    expect(parse_ccl_scratch_option("--local-ccl-scratch", false, local,
                                    error) == 1,
           "Rust backend accepts local scratch option");
    expect(local && error.empty(), "Rust backend selects local scratch");
    expect(parse_ccl_scratch_option("--local-ccl-scratch", false, local,
                                    error) == 1,
           "duplicate local scratch option is idempotent");
    expect(local && error.empty(), "duplicate option keeps local scratch");

    local = false;
    expect(parse_ccl_scratch_option("--local-ccl-scratch", true, local,
                                    error) == -1,
           "C backend rejects local scratch option");
    expect(!local, "rejected C option does not change state");
    expect(error == "--local-ccl-scratch is only valid for apriltag_demo; "
                    "apriltag_c_demo uses the upstream C detector",
           "C backend reports the expected error");

    expect(std::string(ccl_scratch_mode_name(false)) == "reusable",
           "startup names reusable mode");
    expect(std::string(ccl_scratch_mode_name(true)) == "local",
           "startup names local mode");

    reset_fakes();
    void* detector = create_configured_detector(25, false, fake_new, fake_set,
                                                 fake_free);
    expect(detector == reinterpret_cast<void*>(0x1234),
           "configured detector is returned");
    expect(calls == std::vector<std::string>({"new:25", "set"}),
           "scratch setter immediately follows construction");
    expect(setter_mode == APRILTAG_CCL_SCRATCH_MODE_REUSABLE,
           "default configures reusable mode constant");

    reset_fakes();
    detector = create_configured_detector(9, true, fake_new, fake_set,
                                           fake_free);
    expect(detector == reinterpret_cast<void*>(0x1234),
           "local configured detector is returned");
    expect(setter_mode == APRILTAG_CCL_SCRATCH_MODE_LOCAL,
           "option configures local mode constant");

    reset_fakes();
    fail_new = true;
    expect(create_configured_detector(25, false, fake_new, fake_set, fake_free)
               == nullptr,
           "construction failure returns null");
    expect(calls == std::vector<std::string>({"new:25"}),
           "construction failure calls neither setter nor free");

    reset_fakes();
    fail_set = true;
    expect(create_configured_detector(25, true, fake_new, fake_set, fake_free)
               == nullptr,
           "setter failure returns null");
    expect(calls == std::vector<std::string>({"new:25", "set", "free"}),
           "setter failure frees the detector");

    std::cout << "demo option tests passed\n";
    return 0;
}
