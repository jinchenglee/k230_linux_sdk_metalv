#include "nn_profile_plugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include <linux/perf_event.h>

namespace {

struct IndexedFile {
    uint32_t index;
    std::string path;
};

struct Options {
    std::string model;
    std::vector<IndexedFile> inputs;
    std::vector<IndexedFile> goldens;
    int cpu = 0;
    int warmup = 10;
    int iterations = 100;
    int cache_scrub_mib = 0;
    uint64_t seed = 1234;
    double atol = 1.0e-5;
    double rtol = 1.0e-5;
    bool perf = true;
};

[[noreturn]] void fail(const std::string &message)
{
    throw std::runtime_error(message);
}

int parse_int(const char *value, const char *option, int minimum)
{
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!end || *end || parsed < minimum)
        fail(std::string("invalid ") + option + ": " + value);
    return static_cast<int>(parsed);
}
IndexedFile parse_indexed(const char *value, const char *option)
{
    std::string spec(value);
    size_t colon = spec.find(':');
    if (colon == std::string::npos)
        return {0, spec};
    int index = parse_int(spec.substr(0, colon).c_str(), option, 0);
    std::string file = spec.substr(colon + 1);
    if (file.empty())
        fail(std::string("missing file for ") + option);
    return {static_cast<uint32_t>(index), file};
}


void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " --model MODEL.so [options]\n"
        << "  --input [N:]FILE      binary input for tensor N; repeatable\n"
        << "  --golden [N:]FILE     binary golden output for tensor N; repeatable\n"
        << "  --cpu N               pin to CPU N; -1 disables pinning (default 0)\n"
        << "  --warmup N            untimed model calls (default 10)\n"
        << "  --iterations N        measured model calls (default 100)\n"
        << "  --cache-scrub-mib N   touch N MiB before each call (default 0)\n"
        << "  --seed N              deterministic generated-input seed\n"
        << "  --atol X --rtol X     f32 golden tolerances (default 1e-5)\n"
        << "  --no-perf             disable in-process Linux perf counters\n";
}

Options parse_options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (++i >= argc)
                fail("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--model")
            options.model = value();
        else if (arg == "--input")
            options.inputs.push_back(parse_indexed(value(), "--input"));
        else if (arg == "--golden")
            options.goldens.push_back(parse_indexed(value(), "--golden"));
        else if (arg == "--cpu")
            options.cpu = parse_int(value(), "--cpu", -1);
        else if (arg == "--warmup")
            options.warmup = parse_int(value(), "--warmup", 0);
        else if (arg == "--iterations")
            options.iterations = parse_int(value(), "--iterations", 1);
        else if (arg == "--cache-scrub-mib")
            options.cache_scrub_mib = parse_int(value(), "--cache-scrub-mib", 0);
        else if (arg == "--seed")
            options.seed = std::strtoull(value(), nullptr, 0);
        else if (arg == "--atol")
            options.atol = std::strtod(value(), nullptr);
        else if (arg == "--rtol")
            options.rtol = std::strtod(value(), nullptr);
        else if (arg == "--no-perf")
            options.perf = false;
        else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else
            fail("unknown option: " + arg);
    }
    if (options.model.empty())
        fail("--model is required");
    return options;
}

size_t dtype_size(uint32_t dtype)
{
    switch (dtype) {
    case NN_PROFILE_DTYPE_F32:
    case NN_PROFILE_DTYPE_I32:
        return 4;
    case NN_PROFILE_DTYPE_F16:
        return 2;
    case NN_PROFILE_DTYPE_I8:
    case NN_PROFILE_DTYPE_U8:
        return 1;
    case NN_PROFILE_DTYPE_I64:
        return 8;
    default:
        fail("plugin declares unsupported dtype " + std::to_string(dtype));
    }
}

struct Buffer {
    void *data = nullptr;
    size_t bytes = 0;

    explicit Buffer(size_t requested) : bytes(requested)
    {
        if (posix_memalign(&data, 64, std::max<size_t>(requested, 64)) != 0)
            fail("aligned allocation failed");
        std::memset(data, 0, requested);
    }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept : data(other.data), bytes(other.bytes)
    {
        other.data = nullptr;
    }
    ~Buffer() { std::free(data); }
};

void read_exact(const std::string &path, Buffer &buffer)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        fail("cannot open " + path);
    auto size = stream.tellg();
    if (size < 0 || static_cast<size_t>(size) != buffer.bytes)
        fail(path + " has " + std::to_string(size) + " bytes; expected " +
             std::to_string(buffer.bytes));
    stream.seekg(0);
    stream.read(static_cast<char *>(buffer.data), size);
    if (!stream)
        fail("failed to read " + path);
}

uint64_t next_random(uint64_t &state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void fill_input(Buffer &buffer, uint32_t dtype, uint64_t &state)
{
    if (dtype == NN_PROFILE_DTYPE_F32) {
        float *values = static_cast<float *>(buffer.data);
        for (size_t i = 0; i < buffer.bytes / sizeof(float); ++i) {
            uint32_t bits = static_cast<uint32_t>(next_random(state));
            values[i] = static_cast<float>(bits) /
                            static_cast<float>(UINT32_MAX) * 2.0f -
                        1.0f;
        }
    } else {
        uint8_t *values = static_cast<uint8_t *>(buffer.data);
        for (size_t i = 0; i < buffer.bytes; ++i)
            values[i] = static_cast<uint8_t>(next_random(state));
    }
}

uint64_t checksum(const std::vector<Buffer> &buffers)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const auto &buffer : buffers) {
        const uint8_t *bytes = static_cast<const uint8_t *>(buffer.data);
        for (size_t i = 0; i < buffer.bytes; ++i) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

void check_golden(const Buffer &actual, const Buffer &golden, uint32_t dtype,
                  double atol, double rtol)
{
    if (dtype != NN_PROFILE_DTYPE_F32) {
        if (std::memcmp(actual.data, golden.data, actual.bytes) != 0)
            fail("integer golden comparison failed");
        return;
    }
    const float *a = static_cast<const float *>(actual.data);
    const float *g = static_cast<const float *>(golden.data);
    size_t count = actual.bytes / sizeof(float);
    for (size_t i = 0; i < count; ++i) {
        double difference = std::fabs(static_cast<double>(a[i]) - g[i]);
        double threshold = atol + rtol * std::fabs(static_cast<double>(g[i]));
        if (difference > threshold)
            fail("golden mismatch at output[0][" + std::to_string(i) +
                 "]: actual=" + std::to_string(a[i]) +
                 " golden=" + std::to_string(g[i]));
    }
}

struct Counter {
    const char *name;
    uint64_t config;
    int fd = -1;
    double value = 0;
    double running_ratio = 0;
    uint64_t total_enabled = 0;
    uint64_t total_running = 0;

    bool open_counter()
    {
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
        fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
        return fd >= 0;
    }
    void start() const
    {
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    void stop()
    {
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        struct {
            uint64_t raw;
            uint64_t enabled;
            uint64_t running;
        } result{};
        if (read(fd, &result, sizeof(result)) == sizeof(result) && result.running) {
            value += static_cast<double>(result.raw) * result.enabled /
                     result.running;
            total_enabled += result.enabled;
            total_running += result.running;
            running_ratio = total_enabled
                                ? static_cast<double>(total_running) /
                                      total_enabled
                                : 0.0;
        }
    }
    ~Counter()
    {
        if (fd >= 0)
            close(fd);
    }
};

uint64_t read_vlen_bits()
{
#if defined(__riscv) && defined(__riscv_vector)
    uintptr_t bytes = 0;
    __asm__ volatile("csrr %0, vlenb" : "=r"(bytes));
    return static_cast<uint64_t>(bytes) * 8;
#else
    return 0;
#endif
}

void pin_cpu(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fail("sched_setaffinity failed for CPU " + std::to_string(cpu));
}

void scrub_cache(std::vector<uint8_t> &buffer)
{
    static volatile uint64_t sink = 0;
    uint64_t sum = sink;
    for (size_t i = 0; i < buffer.size(); i += 64) {
        buffer[i] = static_cast<uint8_t>(buffer[i] + 1);
        sum += buffer[i];
    }
    sink = sum;
}

double percentile(std::vector<double> sorted, double fraction)
{
    std::sort(sorted.begin(), sorted.end());
    size_t index = static_cast<size_t>(std::ceil(fraction * sorted.size())) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

} // namespace

int main(int argc, char **argv)
{
    try {
        Options options = parse_options(argc, argv);
        pin_cpu(options.cpu);

        void *library = dlopen(options.model.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library)
            fail(std::string("dlopen failed: ") + dlerror());
        auto getter = reinterpret_cast<nn_profile_get_model_v1_fn>(
            dlsym(library, NN_PROFILE_GETTER_SYMBOL));
        if (!getter)
            fail("model plugin does not export " NN_PROFILE_GETTER_SYMBOL);
        const nn_profile_model_v1 *model = getter();
        if (!model || model->abi_version != NN_PROFILE_PLUGIN_ABI_VERSION ||
            model->struct_size < sizeof(*model) || !model->run)
            fail("incompatible model plugin ABI");

        std::vector<Buffer> input_buffers;
        std::vector<Buffer> output_buffers;
        std::vector<nn_profile_tensor_v1> inputs(model->input_count);
        std::vector<nn_profile_tensor_v1> outputs(model->output_count);
        uint64_t random_state = options.seed ? options.seed : 1;
        for (uint32_t i = 0; i < model->input_count; ++i) {
            size_t bytes = model->inputs[i].element_count *
                           dtype_size(model->inputs[i].dtype);
            input_buffers.emplace_back(bytes);
            fill_input(input_buffers.back(), model->inputs[i].dtype, random_state);
            inputs[i] = {input_buffers.back().data, model->inputs[i].element_count};
        }
        for (uint32_t i = 0; i < model->output_count; ++i) {
            size_t bytes = model->outputs[i].element_count *
                           dtype_size(model->outputs[i].dtype);
            output_buffers.emplace_back(bytes);
            outputs[i] = {output_buffers.back().data, model->outputs[i].element_count};
        }
        for (const auto &input : options.inputs) {
            if (input.index >= input_buffers.size())
                fail("--input index is outside the model input range");
            read_exact(input.path, input_buffers[input.index]);
        }

        nn_profile_error_v1 error{};
        auto run_model = [&]() {
            int32_t status = model->run(inputs.data(), model->input_count,
                                        outputs.data(), model->output_count, &error);
            if (status != 0)
                fail("model_run failed status=" + std::to_string(status) +
                     " node=" + std::to_string(error.node_id) +
                     " detail=" + std::to_string(error.detail_status));
        };

        run_model();
        for (const auto &golden_file : options.goldens) {
            if (golden_file.index >= output_buffers.size())
                fail("--golden index is outside the model output range");
            Buffer golden(output_buffers[golden_file.index].bytes);
            read_exact(golden_file.path, golden);
            check_golden(output_buffers[golden_file.index], golden,
                         model->outputs[golden_file.index].dtype,
                         options.atol, options.rtol);
        }
        for (int i = 0; i < options.warmup; ++i)
            run_model();

        std::vector<Counter> counters = {
            {"cycles", PERF_COUNT_HW_CPU_CYCLES},
            {"instructions", PERF_COUNT_HW_INSTRUCTIONS},
            {"cache_references", PERF_COUNT_HW_CACHE_REFERENCES},
            {"cache_misses", PERF_COUNT_HW_CACHE_MISSES},
        };
        if (options.perf) {
            for (auto &counter : counters)
                if (!counter.open_counter())
                    std::cerr << "warning: perf counter unavailable: "
                              << counter.name << "\n";
        }

        std::vector<uint8_t> cache_scrub(
            static_cast<size_t>(options.cache_scrub_mib) * 1024 * 1024);
        std::vector<double> nanoseconds;
        nanoseconds.reserve(options.iterations);
        for (int i = 0; i < options.iterations; ++i) {
            if (!cache_scrub.empty())
                scrub_cache(cache_scrub);
            for (const auto &counter : counters)
                if (counter.fd >= 0)
                    counter.start();
            auto start = std::chrono::steady_clock::now();
            run_model();
            auto stop = std::chrono::steady_clock::now();
            for (auto &counter : counters)
                if (counter.fd >= 0)
                    counter.stop();
            nanoseconds.push_back(
                std::chrono::duration<double, std::nano>(stop - start).count());
        }

        double total_ns = std::accumulate(nanoseconds.begin(), nanoseconds.end(), 0.0);
        double mean_ns = total_ns / nanoseconds.size();
        double median_ns = percentile(nanoseconds, 0.5);
        double p95_ns = percentile(nanoseconds, 0.95);
        double batch = model->static_batch_size ? model->static_batch_size : 1;
        double samples_per_second = batch * 1.0e9 / median_ns;
        double gflops = model->flops_per_inference
                            ? model->flops_per_inference / median_ns
                            : 0.0;

        struct utsname uts{};
        uname(&uts);
        std::cout << std::fixed << std::setprecision(3)
                  << "MODEL name=" << model->model_name
                  << " build=" << model->build_id
                  << " batch=" << model->static_batch_size
                  << " inputs=" << model->input_count
                  << " outputs=" << model->output_count << "\n"
                  << "TARGET machine=" << uts.machine
                  << " kernel=" << uts.release
                  << " cpu=" << options.cpu
                  << " vlen_bits=" << read_vlen_bits()
                  << " cache_scrub_mib=" << options.cache_scrub_mib << "\n"
                  << "RESULT model=" << model->model_name
                  << " batch=" << model->static_batch_size
                  << " calls=" << options.iterations
                  << " warmup=" << options.warmup
                  << " min_ns=" << *std::min_element(nanoseconds.begin(), nanoseconds.end())
                  << " median_ns=" << median_ns
                  << " mean_ns=" << mean_ns
                  << " p95_ns=" << p95_ns
                  << " max_ns=" << *std::max_element(nanoseconds.begin(), nanoseconds.end())
                  << " ns_per_sample=" << median_ns / batch
                  << " samples_per_second=" << samples_per_second
                  << " macs_per_inference=" << model->macs_per_inference
                  << " flops_per_inference=" << model->flops_per_inference
                  << " gflops=" << gflops
                  << " checksum=0x" << std::hex << checksum(output_buffers)
                  << std::dec;
        double cycles = 0;
        double instructions = 0;
        for (const auto &counter : counters) {
            if (counter.fd < 0)
                continue;
            double per_call = counter.value / options.iterations;
            std::cout << " " << counter.name << "_per_inference=" << per_call
                      << " " << counter.name << "_running_ratio="
                      << counter.running_ratio;
            if (counter.config == PERF_COUNT_HW_CPU_CYCLES)
                cycles = per_call;
            if (counter.config == PERF_COUNT_HW_INSTRUCTIONS)
                instructions = per_call;
        }
        if (cycles > 0 && instructions > 0)
            std::cout << " ipc=" << instructions / cycles;
        if (cycles > 0 && model->flops_per_inference)
            std::cout << " flops_per_cycle="
                      << model->flops_per_inference / cycles;
        std::cout << "\n";

        dlclose(library);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "nn_profile: " << error.what() << "\n";
        return 2;
    }
}

