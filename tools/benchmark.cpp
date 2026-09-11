#include "context_hmi/engine.hpp"
#include "context_hmi/context_retrieval.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif

namespace {

using context_hmi::Json;
using context_hmi::resolve_task;

struct Config {
    std::size_t trials = 7;
    std::size_t iterations = 100;
    std::size_t batch_size = 1;
    std::string base_model = "examples/machines/assembly-line.json";
    std::string revision_model = "examples/machines/assembly-line-revision-2.json";
};

struct Statistics {
    double p50 = 0.0;
    double p95 = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
};

std::string require_value(int &index, int argc, char **argv, const char *option) {
    if (index + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + option);
    return argv[++index];
}

std::size_t positive_size(const std::string &text, const char *option) {
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed);
    } catch (const std::exception &) {
        throw std::runtime_error(std::string(option) + " must be a positive integer");
    }
    if (text.empty() || text.front() == '-' || consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error(std::string(option) + " must be a positive integer");
    return static_cast<std::size_t>(value);
}

Config parse_args(int argc, char **argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--trials") {
            config.trials = positive_size(require_value(index, argc, argv, "--trials"), "--trials");
        } else if (option == "--iterations") {
            config.iterations =
                positive_size(require_value(index, argc, argv, "--iterations"), "--iterations");
        } else if (option == "--batch-size") {
            config.batch_size =
                positive_size(require_value(index, argc, argv, "--batch-size"), "--batch-size");
        } else if (option == "--base-model") {
            config.base_model = require_value(index, argc, argv, "--base-model");
        } else if (option == "--revision-model") {
            config.revision_model = require_value(index, argc, argv, "--revision-model");
        } else if (option == "--help") {
            std::cout << "context-hmi-benchmark [--trials N] [--iterations N] [--batch-size N] "
                         "[--base-model FILE] [--revision-model FILE]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + option);
        }
    }
    if (config.trials > 100 || config.iterations > 1000000 || config.batch_size > 10000 ||
        config.trials * config.iterations > 1000000 ||
        config.trials * config.iterations * config.batch_size > 10000000)
        throw std::runtime_error(
            "benchmark is limited to 1 million samples and 10 million operations");
    return config;
}

Json load_model(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open model: " + path);
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    try {
        return Json::parse(body);
    } catch (const std::exception &error) {
        throw std::runtime_error("invalid model " + path + ": " + error.what());
    }
}

std::string date_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string operating_system() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "unknown";
#endif
}

std::string architecture() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

std::string compiler_version() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

double percentile(const std::vector<double> &sorted, double fraction) {
    if (sorted.empty())
        return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] + weight * (sorted[upper] - sorted[lower]);
}

Statistics summarize(std::vector<double> samples) {
    if (samples.empty())
        throw std::runtime_error("benchmark produced no samples");
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const double sample : samples)
        total += sample;
    return {percentile(samples, 0.50), percentile(samples, 0.95), samples.front(), samples.back(),
            total / static_cast<double>(samples.size())};
}

template <typename Operation>
std::vector<double> measure(const Config &config, Operation operation) {
    std::vector<double> samples;
    samples.reserve(config.trials * config.iterations);
    for (std::size_t trial = 0; trial < config.trials; ++trial) {
        for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t item = 0; item < config.batch_size; ++item)
                operation();
            const auto finish = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double, std::milli>(finish - start).count();
            samples.push_back(elapsed / static_cast<double>(config.batch_size));
        }
    }
    return samples;
}

Json statistics_json(const Statistics &statistics) {
    return Json{{"p50_ms", statistics.p50},
                {"p95_ms", statistics.p95},
                {"min_ms", statistics.minimum},
                {"max_ms", statistics.maximum},
                {"mean_ms", statistics.mean}};
}

Json workload_json(const Json &model, const std::string &path) {
    const auto array_size = [&model](const char *key) -> std::size_t {
        return model.contains(key) && model.at(key).is_array() ? model.at(key).size() : 0;
    };
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return Json{{"path", path},
                {"model_id", model.value("model_id", "")},
                {"revision", model.value("revision", 0)},
                {"bytes", error ? 0 : bytes},
                {"assets", array_size("assets")},
                {"relationships", array_size("relationships")},
                {"tags", array_size("tags")},
                {"alarms", array_size("alarms")}};
}

Json metric_json(const char *name, const char *operation, const std::vector<double> &samples,
                 const Config &config) {
    Json trials = Json::array();
    for (std::size_t trial = 0; trial < config.trials; ++trial) {
        const auto first = samples.begin() + static_cast<std::ptrdiff_t>(trial * config.iterations);
        std::vector<double> slice(first, first + static_cast<std::ptrdiff_t>(config.iterations));
        trials.push_back(statistics_json(summarize(slice)));
    }
    return Json{{"name", name},
                {"operation", operation},
                {"sample_count", samples.size()},
                {"samples_ms", samples},
                {"trials", trials},
                {"times_ms", statistics_json(summarize(samples))}};
}

Json cpu_availability() {
    Json result{{"hardware_concurrency", std::thread::hardware_concurrency()},
                {"process_affinity", "unknown"}};
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
        result["process_affinity"] = CPU_COUNT(&allowed);
#endif
    return result;
}

}  /* namespace */

int main(int argc, char **argv) {
    try {
        const Config config = parse_args(argc, argv);
        const Json base_model = load_model(config.base_model);
        const Json revision_model = load_model(config.revision_model);
        const std::string prompt = "Show the process temperature for the cure oven";
        const Json retrieval = context_hmi::context::retrieve(base_model, prompt);
        if (retrieval.value("requires_clarification", true) ||
            retrieval.at("eligible_asset_ids").size() != 1)
            throw std::runtime_error("base prompt did not retrieve one equipment scope");
        const Json base_task{{"kind", "overview"},
                             {"model_id", base_model.at("model_id")},
                             {"model_revision", base_model.at("revision")},
                             {"original_request", prompt},
                             {"anchor_asset_id", retrieval.at("eligible_asset_ids").at(0)},
                             {"measurement_roles", retrieval.at("measurement_role_hints")}};
        const Json previous_view = resolve_task(base_model, base_task);
        std::size_t benchmark_guard = 0;

        const auto retrieve_resolve_samples = measure(config, [&]() {
            const Json current_retrieval = context_hmi::context::retrieve(base_model, prompt);
            Json current_task = base_task;
            current_task["anchor_asset_id"] = current_retrieval.at("eligible_asset_ids").at(0);
            const Json current_view = resolve_task(base_model, current_task);
            benchmark_guard += current_view.size();
        });
        const auto reconcile_samples = measure(config, [&]() {
            const Json reconciled = context_hmi::reconcile_view(revision_model, previous_view);
            benchmark_guard += reconciled.size();
        });

        const auto base_workload = workload_json(base_model, config.base_model);
        const auto revision_workload = workload_json(revision_model, config.revision_model);
        Json output{
            {"schema_version", 1},
            {"benchmark", "context-hmi native context baseline"},
            {"date_utc", date_utc()},
            {"environment", Json{{"os", operating_system()},
                                 {"architecture", architecture()},
                                 {"cpp_compiler", compiler_version()},
                                 {"cpp_standard", std::to_string(__cplusplus)},
                                 {"runtime", "C++ standard library"},
                                 {"cpu_availability", cpu_availability()}}},
            {"config", Json{{"trials", config.trials},
                            {"iterations", config.iterations},
                            {"batch_size", config.batch_size},
                            {"sample_count", config.trials * config.iterations},
                            {"build_type", BENCHMARK_BUILD_TYPE},
                            {"prompt", prompt},
                            {"timing_clock", "steady_clock"}}},
            {"workload", Json{{"base_model", base_workload},
                              {"revision_model", revision_workload},
                              {"previous_view_status", previous_view.value("status", "")}}},
            {"metrics",
             Json::array({metric_json("retrieve_resolve", "context retrieval + resolve_task",
                                      retrieve_resolve_samples, config),
                          metric_json("reconcile", "reconcile_view", reconcile_samples, config)})},
            {"checks", Json{{"retrieval_requires_clarification",
                              retrieval.value("requires_clarification", true)},
                            {"base_view_status", previous_view.value("status", "")},
                            {"guard", benchmark_guard},
                            {"counts_as_product_comparison", false},
                            {"excluded", Json::array({"GUI rendering", "llama.cpp inference",
                                                      "external source-adapter I/O",
                                                      "network latency"})}}}};
        std::cout << output.dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "context-hmi-benchmark: " << error.what() << '\n';
        return 2;
    }
}
