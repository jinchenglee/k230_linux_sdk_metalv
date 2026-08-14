#include "sequence.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace apriltag_bench {
namespace {

constexpr std::uint32_t kShaInitial[] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
constexpr std::uint32_t kShaRound[] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

std::uint32_t rotate_right(std::uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32 - shift));
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) close(value_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

struct ReadFile {
    FileIdentity identity;
    std::vector<std::uint8_t> bytes;
};

ReadFile read_regular_file(const std::string& path)
{
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) throw InputError("cannot securely open sequence input: " + path);
    struct stat status{};
    if (fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode))
        throw InputError("sequence input must be a regular non-symlink file: " + path);
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) >
                                std::numeric_limits<std::size_t>::max())
        throw InputError("sequence input is too large: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw InputError("cannot read complete sequence input: " + path);
        offset += static_cast<std::size_t>(count);
    }
    char proc_path[64];
    const int proc_length = std::snprintf(proc_path, sizeof(proc_path),
                                          "/proc/self/fd/%d", file.get());
    if (proc_length <= 0 || static_cast<std::size_t>(proc_length) >= sizeof(proc_path))
        throw InputError("cannot identify sequence input: " + path);
    std::vector<char> resolved(4096);
    const ssize_t resolved_length = readlink(proc_path, resolved.data(), resolved.size() - 1);
    if (resolved_length < 0 || static_cast<std::size_t>(resolved_length) == resolved.size() - 1)
        throw InputError("cannot resolve sequence input descriptor: " + path);
    resolved[static_cast<std::size_t>(resolved_length)] = '\0';
    FileIdentity identity{path, resolved.data(), static_cast<std::uint64_t>(status.st_dev),
                          static_cast<std::uint64_t>(status.st_ino), sha256_hex(bytes)};
    return {std::move(identity), std::move(bytes)};
}

std::size_t parse_dimension(const std::string& text, std::size_t line)
{
    std::size_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        value == 0 || value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw InputError("invalid sequence manifest dimension at line " + std::to_string(line));
    return value;
}

void emit_buffer(std::ostream& out, const char* name,
                 const apriltag_buffer_telemetry_t& buffer)
{
    out << ' ' << name << "_len=" << buffer.len
        << ' ' << name << "_capacity=" << buffer.capacity
        << ' ' << name << "_high_water=" << buffer.high_water
        << ' ' << name << "_growths_call=" << buffer.growths_call
        << ' ' << name << "_growths_total=" << buffer.growths_total
        << ' ' << name << "_capacity_bytes=" << buffer.capacity_bytes
        << ' ' << name << "_high_water_bytes=" << buffer.high_water_bytes
        << ' ' << name << "_element_size=" << buffer.element_size;
}

void validate_buffer(const apriltag_buffer_telemetry_t& buffer)
{
    if (buffer.len > buffer.capacity || buffer.high_water > buffer.capacity ||
        buffer.high_water < buffer.len || buffer.element_size == 0 ||
        buffer.capacity > std::numeric_limits<std::uint64_t>::max() / buffer.element_size ||
        buffer.high_water > std::numeric_limits<std::uint64_t>::max() / buffer.element_size ||
        buffer.capacity_bytes != buffer.capacity * buffer.element_size ||
        buffer.high_water_bytes != buffer.high_water * buffer.element_size ||
        buffer.growths_call > buffer.growths_total)
        throw std::runtime_error("invalid CCL scratch buffer invariants");
}

void validate_cold_buffer(const apriltag_buffer_telemetry_t& current,
                          const apriltag_buffer_telemetry_t& previous)
{
    if (current.capacity < previous.capacity ||
        current.high_water < previous.high_water ||
        current.growths_total < previous.growths_total ||
        current.element_size != previous.element_size)
        throw std::runtime_error("CCL scratch regressed across sequence rows");
}

void validate_warm_buffer(const apriltag_buffer_telemetry_t& current,
                          const apriltag_buffer_telemetry_t& cold)
{
    if (current.growths_call != 0 || current.len != cold.len ||
        current.capacity != cold.capacity ||
        current.high_water != cold.high_water ||
        current.growths_total != cold.growths_total ||
        current.capacity_bytes != cold.capacity_bytes ||
        current.high_water_bytes != cold.high_water_bytes ||
        current.element_size != cold.element_size)
        throw std::runtime_error("CCL scratch changed during warm sequence repetitions");
}

void validate_local_buffer(const apriltag_buffer_telemetry_t& current)
{
    if (current.growths_total != current.growths_call)
        throw std::runtime_error("local CCL scratch growth total is not per-call");
}

void validate_local_repeated_buffer(const apriltag_buffer_telemetry_t& current,
                                     const apriltag_buffer_telemetry_t& first)
{
    if (current.len != first.len || current.capacity != first.capacity ||
        current.high_water != first.high_water ||
        current.capacity_bytes != first.capacity_bytes ||
        current.high_water_bytes != first.high_water_bytes ||
        current.element_size != first.element_size)
        throw std::runtime_error("local CCL scratch changed within sequence row");
}

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right,
                          const char* context)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        throw std::runtime_error(std::string("CCL pending profile ") + context);
    return left + right;
}

std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right,
                               const char* context)
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        throw std::runtime_error(std::string("CCL pending profile ") + context);
    return left * right;
}
}

std::string sha256_hex(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint8_t> padded(bytes);
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max() >> 3))
        throw InputError("input is too large for SHA-256");
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) padded.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        padded.push_back(static_cast<std::uint8_t>(bits >> shift));

    std::array<std::uint32_t, 8> hash;
    std::copy(std::begin(kShaInitial), std::end(kShaInitial), hash.begin());
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::uint32_t words[64]{};
        for (int i = 0; i < 16; ++i) {
            const std::size_t at = offset + static_cast<std::size_t>(i) * 4;
            words[i] = (static_cast<std::uint32_t>(padded[at]) << 24) |
                       (static_cast<std::uint32_t>(padded[at + 1]) << 16) |
                       (static_cast<std::uint32_t>(padded[at + 2]) << 8) |
                       padded[at + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(words[i - 15], 7) ^
                                     rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(words[i - 2], 17) ^
                                     rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto state = hash;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotate_right(state[4], 6) ^ rotate_right(state[4], 11) ^
                                     rotate_right(state[4], 25);
            const std::uint32_t choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
            const std::uint32_t temp1 = state[7] + s1 + choice + kShaRound[i] + words[i];
            const std::uint32_t s0 = rotate_right(state[0], 2) ^ rotate_right(state[0], 13) ^
                                     rotate_right(state[0], 22);
            const std::uint32_t majority = (state[0] & state[1]) ^ (state[0] & state[2]) ^
                                           (state[1] & state[2]);
            const std::uint32_t temp2 = s0 + majority;
            for (int j = 7; j > 0; --j) state[j] = state[j - 1];
            state[4] += temp1;
            state[0] = temp1 + temp2;
        }
        for (std::size_t i = 0; i < hash.size(); ++i) hash[i] += state[i];
    }
    std::ostringstream out;
    for (const auto word : hash) out << std::hex << std::setfill('0') << std::setw(8) << word;
    return out.str();
}

FileIdentity snapshot_regular_file(const std::string& path)
{
    return read_regular_file(path).identity;
}

std::vector<std::uint8_t> read_verified_file(const FileIdentity& identity)
{
    auto current = read_regular_file(identity.path);
    if (current.identity.canonical_path != identity.canonical_path ||
        current.identity.device != identity.device || current.identity.inode != identity.inode ||
        current.identity.sha256 != identity.sha256)
        throw InputError("file changed during sequence run: " + identity.path);
    return std::move(current.bytes);
}

void verify_file_snapshot(const FileIdentity& snapshot)
{
    (void)read_verified_file(snapshot);
}

std::vector<SequenceRow> parse_sequence_manifest(std::istream& input)
{
    std::vector<SequenceRow> rows;
    std::set<std::string> labels;
    std::string line;
    for (std::size_t line_number = 1; std::getline(input, line); ++line_number) {
        if (!line.empty() && line.back() == '\r')
            throw InputError("carriage return in sequence manifest at line " +
                             std::to_string(line_number));
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = line.find('\t', begin);
            fields.push_back(line.substr(begin, end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (fields.size() != 5)
            throw InputError("sequence manifest requires exactly five TSV fields at line " +
                             std::to_string(line_number));
        for (const auto& field : fields)
            if (field.find('\0') != std::string::npos)
                throw InputError("NUL in sequence manifest at line " +
                                 std::to_string(line_number));
        const std::string& label = fields[0];
        if (label.empty() || label == "." || label == ".." ||
            label.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
                std::string::npos)
            throw InputError("unsafe sequence label at line " + std::to_string(line_number));
        if (fields[1].empty() || fields[1].find('\n') != std::string::npos ||
            fields[1].find('\r') != std::string::npos)
            throw InputError("invalid sequence path at line " + std::to_string(line_number));
        if (fields[4].size() != 64 ||
            fields[4].find_first_not_of("0123456789abcdef") != std::string::npos)
            throw InputError("invalid lowercase SHA-256 at line " + std::to_string(line_number));
        if (!labels.insert(label).second)
            throw InputError("duplicate sequence label: " + label);
        rows.push_back({label, fields[1], parse_dimension(fields[2], line_number),
                        parse_dimension(fields[3], line_number), fields[4]});
    }
    if (rows.empty()) throw InputError("sequence manifest is empty");
    return rows;
}

std::vector<SequenceInput> load_sequence_inputs(const std::vector<SequenceRow>& rows)
{
    std::vector<SequenceInput> inputs;
    std::map<std::pair<std::uint64_t, std::uint64_t>, SequenceRow> identities;
    for (const auto& row : rows) {
        auto file = read_regular_file(row.path);
        const auto identity = std::make_pair(file.identity.device, file.identity.inode);
        const auto existing = identities.find(identity);
        if (existing != identities.end() &&
            (existing->second.width != row.width || existing->second.height != row.height ||
             existing->second.sha256 != row.sha256))
            throw InputError("conflicting metadata for repeated sequence input: " +
                              file.identity.canonical_path);
        if (file.identity.sha256 != row.sha256)
            throw InputError("sequence input SHA-256 mismatch: " + row.path);
        identities.emplace(identity, row);
        inputs.push_back({row, std::move(file.identity)});
    }
    return inputs;
}

void verify_sequence_inputs_unchanged(const std::vector<SequenceInput>& inputs)
{
    for (const auto& input : inputs) {
        (void)read_verified_file(input.file);
    }
}

void validate_sequence_scratch(const apriltag_ccl_scratch_v1_t& scratch)
{
    if (scratch.version != APRILTAG_CCL_SCRATCH_VERSION ||
        scratch.struct_size != sizeof(scratch) ||
        scratch.validity != APRILTAG_CCL_SCRATCH_VALID_ALL)
        throw std::runtime_error("incompatible or invalid CCL scratch schema");
    validate_buffer(scratch.pending);
    validate_buffer(scratch.diagonal_left);
    validate_buffer(scratch.diagonal_right);
}

void validate_sequence_pending_profile(
    const apriltag_ccl_pending_profile_v1_t& pending,
    const apriltag_ccl_profile_t& profile)
{
    if (pending.version != APRILTAG_CCL_PENDING_PROFILE_VERSION ||
        pending.struct_size != sizeof(pending) ||
        (pending.validity & ~APRILTAG_CCL_PENDING_PROFILE_VALID_ALL) != 0 ||
        pending.validity != APRILTAG_CCL_PENDING_PROFILE_VALID_ALL ||
        pending.sample_stride != APRILTAG_CCL_PENDING_SAMPLE_STRIDE)
        throw std::runtime_error("incompatible or invalid CCL pending profile schema");
    if (pending.reserved_u32 != 0)
        throw std::runtime_error("CCL pending profile reserved_u32 is nonzero");
    for (const auto value : pending.reserved)
        if (value != 0)
            throw std::runtime_error("CCL pending profile reserved array is nonzero");

    std::uint64_t total_records = 0;
    std::uint64_t total_units = 0;
    std::uint64_t total_accepted = 0;
    std::uint64_t total_root_equal = 0;
    std::uint64_t total_small_component = 0;
    for (std::size_t type = 0; type < APRILTAG_CCL_PENDING_BOUNDARY_TYPES; ++type) {
        std::uint64_t histogram_records = 0;
        for (std::size_t bin = 0; bin < APRILTAG_CCL_PENDING_RANGE_BINS; ++bin)
            histogram_records = checked_add(histogram_records,
                pending.range_histogram[type][bin], "histogram sum overflow");
        const std::uint64_t rejected_records = checked_add(
            pending.root_equal_by_type[type], pending.small_component_by_type[type],
            "outcome sum overflow");
        const std::uint64_t outcome_records = checked_add(
            pending.accepted_records_by_type[type], rejected_records,
            "outcome sum overflow");
        if (histogram_records != pending.pending_records_by_type[type] ||
            outcome_records != pending.pending_records_by_type[type] ||
            pending.accepted_units_by_type[type] > pending.pending_units_by_type[type] ||
            pending.pending_records_by_type[type] != profile.pending_by_type[type] ||
            pending.pending_units_by_type[type] !=
                profile.pending_expanded_points_by_type[type] ||
            pending.accepted_units_by_type[type] != profile.emitted_by_type[type])
            throw std::runtime_error("invalid CCL pending profile conservation");
        total_records = checked_add(total_records, pending.pending_records_by_type[type],
                                    "global sum overflow: pending records");
        total_units = checked_add(total_units, pending.pending_units_by_type[type],
                                  "global sum overflow: pending units");
        total_accepted = checked_add(total_accepted,
            pending.accepted_records_by_type[type],
            "global sum overflow: accepted records");
        total_root_equal = checked_add(total_root_equal,
            pending.root_equal_by_type[type], "global sum overflow: root equal");
        total_small_component = checked_add(total_small_component,
            pending.small_component_by_type[type],
            "global sum overflow: small component");
    }
    const std::uint64_t expected_sampled_records = checked_add(
        total_records / APRILTAG_CCL_PENDING_SAMPLE_STRIDE,
        total_records % APRILTAG_CCL_PENDING_SAMPLE_STRIDE != 0,
        "sampled record ceiling overflow");
    const std::uint64_t sampled_intervals = checked_multiply(
        pending.sampled_records, 2, "timer interval overflow");
    const std::uint64_t accepted_intervals = checked_multiply(
        pending.sampled_accepted_records, 2, "timer interval overflow");
    const std::uint64_t expected_timer_intervals = checked_add(
        sampled_intervals, accepted_intervals, "timer interval overflow");
    if (total_root_equal != profile.root_equal_rejects ||
        total_small_component != profile.small_component_rejects ||
        total_accepted != profile.accepted_grouping_records ||
        pending.sampled_records != expected_sampled_records ||
        pending.sampled_units < pending.sampled_records ||
        pending.sampled_units > total_units ||
        pending.sampled_accepted_records > pending.sampled_records ||
        pending.sampled_accepted_records > total_accepted ||
        pending.timer_intervals != expected_timer_intervals)
        throw std::runtime_error("invalid CCL pending profile totals");
}

void SequenceScratchValidator::accept_cold(const apriltag_ccl_scratch_v1_t& scratch)
{
    validate_sequence_scratch(scratch);
    if (mode_ == ScratchMode::Local) {
        validate_local_buffer(scratch.pending);
        validate_local_buffer(scratch.diagonal_left);
        validate_local_buffer(scratch.diagonal_right);
    } else if (have_previous_) {
        validate_cold_buffer(scratch.pending, previous_.pending);
        validate_cold_buffer(scratch.diagonal_left, previous_.diagonal_left);
        validate_cold_buffer(scratch.diagonal_right, previous_.diagonal_right);
    }
    row_cold_ = scratch;
    previous_ = scratch;
    have_previous_ = true;
    have_row_cold_ = true;
}

void SequenceScratchValidator::accept_warm(const apriltag_ccl_scratch_v1_t& scratch)
{
    if (!have_row_cold_)
        throw std::runtime_error("warm sequence scratch has no cold baseline");
    validate_sequence_scratch(scratch);
    if (mode_ == ScratchMode::Local) {
        validate_local_buffer(scratch.pending);
        validate_local_buffer(scratch.diagonal_left);
        validate_local_buffer(scratch.diagonal_right);
        validate_local_repeated_buffer(scratch.pending, row_cold_.pending);
        validate_local_repeated_buffer(scratch.diagonal_left, row_cold_.diagonal_left);
        validate_local_repeated_buffer(scratch.diagonal_right, row_cold_.diagonal_right);
    } else {
        validate_warm_buffer(scratch.pending, row_cold_.pending);
        validate_warm_buffer(scratch.diagonal_left, row_cold_.diagonal_left);
        validate_warm_buffer(scratch.diagonal_right, row_cold_.diagonal_right);
    }
    previous_ = scratch;
}

void print_sequence_record(const SequenceRecord& record, double factor,
                           std::uint32_t min_blob, std::uint64_t rvv_mask,
                           const std::string& stages, const std::string& build_id,
                           std::ostream& out)
{
    out << "SEQUENCE index=" << record.index << " label=" << record.label << " phase=" << record.phase
        << " repetition=" << record.repetition
        << " scratch_mode=" << (record.scratch_mode == ScratchMode::Local ? "local" : "reusable")
        << " width=" << record.width
        << " height=" << record.height << " input_sha256=" << record.file_sha256
        << " manifest_sha256=" << record.manifest_sha256
        << " latency_ns=" << record.elapsed_ns << " detections=" << record.result.count
        << " checksum=" << std::hex << record.result.checksum << std::dec
        << " factor=" << factor << " min_blob=" << min_blob
        << " rvv_mask=0x" << std::hex << rvv_mask << std::dec
        << " stages=" << stages << " build=" << build_id
        << " profile_version=" << record.profile.version
        << " profile_struct_size=" << record.profile.struct_size
        << " scratch_struct_size=" << record.scratch.struct_size
        << " profile_validity=0x" << std::hex << record.profile.validity << std::dec
        << " scratch_version=" << record.scratch.version
        << " scratch_validity=0x" << std::hex << record.scratch.validity << std::dec
        << " ccl_total_ns=" << record.profile.total_ns
        << " ccl_group_emit_ns=" << record.profile.group_emit_ns
        << " ccl_root_materialize_ns=" << record.profile.root_materialize_ns
        << " ccl_pending_growths=" << record.profile.pending_growths
        << " pending_profile_version=" << record.pending_profile.version
        << " pending_profile_struct_size=" << record.pending_profile.struct_size
        << " pending_profile_validity=0x" << std::hex << record.pending_profile.validity << std::dec
        << " pending_sample_stride=" << record.pending_profile.sample_stride
        << " pending_sampled_records=" << record.pending_profile.sampled_records
        << " pending_sampled_units=" << record.pending_profile.sampled_units
        << " pending_sampled_accepted_records="
        << record.pending_profile.sampled_accepted_records
        << " pending_timer_intervals=" << record.pending_profile.timer_intervals
        << " pending_timer_overhead_ns=" << record.pending_profile.timer_overhead_ns
        << " pending_construct_sample_ns=" << record.pending_profile.construct_sample_ns
        << " pending_resolve_sample_ns=" << record.pending_profile.resolve_sample_ns
        << " pending_lookup_sample_ns=" << record.pending_profile.lookup_sample_ns
        << " pending_emit_sample_ns=" << record.pending_profile.emit_sample_ns;
    for (std::size_t type = 0; type < APRILTAG_CCL_PENDING_BOUNDARY_TYPES; ++type) {
        out << " pending_records_type_" << type << '='
            << record.pending_profile.pending_records_by_type[type]
            << " pending_units_type_" << type << '='
            << record.pending_profile.pending_units_by_type[type]
            << " pending_accepted_records_type_" << type << '='
            << record.pending_profile.accepted_records_by_type[type]
            << " pending_accepted_units_type_" << type << '='
            << record.pending_profile.accepted_units_by_type[type]
            << " pending_root_equal_type_" << type << '='
            << record.pending_profile.root_equal_by_type[type]
            << " pending_small_component_type_" << type << '='
            << record.pending_profile.small_component_by_type[type];
        for (std::size_t bin = 0; bin < APRILTAG_CCL_PENDING_RANGE_BINS; ++bin)
            out << " pending_range_type_" << type << "_bin_" << bin << '='
                << record.pending_profile.range_histogram[type][bin];
    }
    emit_buffer(out, "pending", record.scratch.pending);
    emit_buffer(out, "diagonal_left", record.scratch.diagonal_left);
    emit_buffer(out, "diagonal_right", record.scratch.diagonal_right);
    out << '\n';
}

void run_sequence(const std::vector<SequenceInput>& inputs,
                  const LoadPreparedSequenceImage& load_image, int warm_reps,
                  const std::string& manifest_sha256, double factor,
                   std::uint32_t min_blob, std::uint64_t rvv_mask,
                   const std::string& stages, const std::string& build_id,
                   ScratchMode scratch_mode,
                  const SequenceCallbacks& callbacks, std::ostream& out)
{
    if (warm_reps < 0 || !load_image || !callbacks.detect || !callbacks.get_profile ||
        !callbacks.get_pending_profile || !callbacks.get_scratch || !callbacks.now_ns ||
        !callbacks.verify_unchanged)
        throw std::invalid_argument("incomplete sequence orchestration configuration");
    SequenceScratchValidator scratch_validator(scratch_mode);
    std::vector<SequenceRecord> all_records;
    std::map<std::string, DetectionResult> expected_by_identity;
    std::size_t sequence_index = 0;
    for (const auto& input : inputs) {
        PreparedImage image = load_image(input);
        DetectionResult expected{};
        std::vector<SequenceRecord> records;
        std::vector<apriltag_ccl_profile_t> profiles;
        records.reserve(static_cast<std::size_t>(warm_reps) + 1);
        profiles.reserve(static_cast<std::size_t>(warm_reps) + 1);
        for (int repetition = 0; repetition <= warm_reps; ++repetition) {
            SequenceRecord record;
            record.scratch_mode = scratch_mode;
            record.index = sequence_index++;
            record.label = input.row.label;
            record.phase = repetition == 0 ? "cold" : "warm";
            record.repetition = repetition;
            record.width = image.width;
            record.height = image.height;
            record.file_sha256 = input.row.sha256;
            record.manifest_sha256 = manifest_sha256;
            const std::uint64_t start = callbacks.now_ns();
            record.result = callbacks.detect(image);
            const std::uint64_t finish = callbacks.now_ns();
            if (finish < start) throw std::runtime_error("sequence clock moved backwards");
            record.elapsed_ns = finish - start;
            record.profile = callbacks.get_profile();
            record.pending_profile = callbacks.get_pending_profile();
            record.scratch = callbacks.get_scratch();
            validate_sequence_pending_profile(record.pending_profile, record.profile);
            if ((record.profile.validity & APRILTAG_CCL_PROFILE_VALID_GROWTH) != 0 &&
                (record.scratch.validity & APRILTAG_CCL_SCRATCH_VALID_BUFFERS) != 0 &&
                record.profile.pending_growths != record.scratch.pending.growths_call)
                throw std::runtime_error("CCL pending growth telemetry disagrees between getters");
            if ((record.profile.validity & APRILTAG_CCL_PROFILE_VALID_TIMINGS) == 0)
                throw std::runtime_error("sequence CCL timing profile is invalid");
            if (repetition == 0) {
                expected = record.result;
                scratch_validator.accept_cold(record.scratch);
            } else {
                if (record.result.count != expected.count ||
                    record.result.checksum != expected.checksum)
                    throw std::runtime_error("unstable detector output for sequence row: " +
                                             input.row.label);
                scratch_validator.accept_warm(record.scratch);
            }
            profiles.push_back(record.profile);
            records.push_back(record);
        }
        validate_sequence_profile_sequence(profiles, scratch_mode);
        const std::string identity = std::to_string(input.file.device) + ":" +
                                     std::to_string(input.file.inode) + ":" +
                                     input.file.sha256;
        const auto previous = expected_by_identity.find(identity);
        if (previous != expected_by_identity.end() &&
            (previous->second.count != expected.count ||
             previous->second.checksum != expected.checksum))
            throw std::runtime_error("detector output changed when sequence input repeated: " +
                                     input.row.label);
        expected_by_identity.emplace(identity, expected);
        all_records.insert(all_records.end(), records.begin(), records.end());
    }
    callbacks.verify_unchanged();
    for (const auto& record : all_records)
        print_sequence_record(record, factor, min_blob, rvv_mask, stages, build_id, out);
}

}  // namespace apriltag_bench
