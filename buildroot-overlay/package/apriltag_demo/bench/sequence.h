#ifndef K230_APRILTAG_SEQUENCE_H
#define K230_APRILTAG_SEQUENCE_H

#include "benchmark.h"
#include "apriltag_pending_profile.h"

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace apriltag_bench {

struct SequenceRow {
    std::string label;
    std::string path;
    std::size_t width;
    std::size_t height;
    std::string sha256;
};

struct FileIdentity {
    std::string path;
    std::string canonical_path;
    std::uint64_t device;
    std::uint64_t inode;
    std::string sha256;
};

struct SequenceInput {
    SequenceRow row;
    FileIdentity file;
};

struct SequenceRecord {
    ScratchMode scratch_mode = ScratchMode::Reusable;
    std::size_t index = 0;
    std::string label;
    std::string phase;
    int repetition = 0;
    std::size_t width = 0;
    std::size_t height = 0;
    std::string file_sha256;
    std::string manifest_sha256;
    std::uint64_t elapsed_ns = 0;
    DetectionResult result;
    apriltag_ccl_profile_t profile{};
    apriltag_ccl_pending_profile_v1_t pending_profile{};
    apriltag_ccl_scratch_v1_t scratch{};
};

struct SequenceCallbacks {
    std::function<DetectionResult(const PreparedImage&)> detect;
    std::function<apriltag_ccl_profile_t()> get_profile;
    std::function<apriltag_ccl_pending_profile_v1_t()> get_pending_profile;
    std::function<apriltag_ccl_scratch_v1_t()> get_scratch;
    std::function<std::uint64_t()> now_ns;
    std::function<void()> verify_unchanged;
};

class SequenceScratchValidator {
public:
    explicit SequenceScratchValidator(ScratchMode mode = ScratchMode::Reusable)
        : mode_(mode) {}
    void accept_cold(const apriltag_ccl_scratch_v1_t& scratch);
    void accept_warm(const apriltag_ccl_scratch_v1_t& scratch);

private:
    ScratchMode mode_;
    bool have_previous_ = false;
    bool have_row_cold_ = false;
    apriltag_ccl_scratch_v1_t previous_{};
    apriltag_ccl_scratch_v1_t row_cold_{};
};

std::string sha256_hex(const std::vector<std::uint8_t>& bytes);
FileIdentity snapshot_regular_file(const std::string& path);
std::vector<std::uint8_t> read_verified_file(const FileIdentity& identity);
void verify_file_snapshot(const FileIdentity& snapshot);
std::vector<SequenceRow> parse_sequence_manifest(std::istream& input);
std::vector<SequenceInput> load_sequence_inputs(const std::vector<SequenceRow>& rows);
void verify_sequence_inputs_unchanged(const std::vector<SequenceInput>& inputs);
void validate_sequence_scratch(const apriltag_ccl_scratch_v1_t& scratch);
void validate_sequence_pending_profile(
    const apriltag_ccl_pending_profile_v1_t& pending,
    const apriltag_ccl_profile_t& profile);
void print_sequence_record(const SequenceRecord& record, double factor,
                           std::uint32_t min_blob, std::uint64_t rvv_mask,
                           const std::string& stages, const std::string& build_id,
                           std::ostream& out);
using LoadPreparedSequenceImage = std::function<PreparedImage(const SequenceInput&)>;
void run_sequence(const std::vector<SequenceInput>& inputs,
                  const LoadPreparedSequenceImage& load_image, int warm_reps,
                  const std::string& manifest_sha256, double factor,
                  std::uint32_t min_blob, std::uint64_t rvv_mask,
                  const std::string& stages, const std::string& build_id,
                  ScratchMode scratch_mode, const SequenceCallbacks& callbacks,
                  std::ostream& out);
int sequence_main(int argc, const char* const argv[], std::ostream& out,
                  std::ostream& err);
using LoadSequenceImage = std::function<PreparedImage(
    const std::vector<std::uint8_t>&, ImageSize)>;
int sequence_cli(int argc, const char* const argv[], const LoadSequenceImage& load_image,
                 std::ostream& out, std::ostream& err);

}  // namespace apriltag_bench

#endif
