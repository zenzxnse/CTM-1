/*
 * Structured diagnostic logger contract tests.
 *
 * Diagnostic output is intentionally separate from required operational records. Low-severity
 * messages may be dropped when bounded, while shutdown must drain accepted records.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#include "context_hmi/diagnostic_logger.hpp"
#include "support/check.hpp"

using context_hmi::diagnostics::Logger;
using context_hmi::diagnostics::LoggerConfig;
using context_hmi::diagnostics::Severity;
using context_hmi_test::check;

namespace {

class ScratchDirectory final {
 public:
    ScratchDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("context-hmi-logger-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path &path() const { return path_; }

 private:
    std::filesystem::path path_;
};

void invalid_logger_configuration_and_events_are_rejected() {
    bool rejected = false;
    try {
        Logger logger(LoggerConfig{{}, 1, 4095, 1});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "a logger with a sub-minimum rotation bound is rejected");

    Logger logger(LoggerConfig{});
    rejected = false;
    try {
        logger.write(Severity::kInfo, "");
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "an empty diagnostic event is rejected");
    logger.shutdown();
}

void accepted_records_are_structured_and_drained_on_shutdown() {
    ScratchDirectory directory;
    const auto path = directory.path() / "diagnostics.jsonl";
    Logger logger(LoggerConfig{path, 4, 4096, 2});
    logger.write(Severity::kInfo, "request_started",
                 nlohmann::json{{"request_id", "req-1"}});
    logger.write(Severity::kWarning, "binding_missing",
                 nlohmann::json{{"tag_id", "line.rate"}});
    logger.shutdown();
    logger.shutdown();

    const auto snapshot = logger.snapshot();
    check(snapshot.at("enabled").get<bool>(), "a configured logger is enabled");
    check(!snapshot.at("accepting").get<bool>(), "shutdown closes admission");
    check(snapshot.at("accepted") == 2, "accepted diagnostic count is recorded");
    check(snapshot.at("written") == 2, "shutdown drains accepted diagnostics");
    check(snapshot.at("failed") == false, "successful writes do not report failure");

    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    check(content.find("request_started") != std::string::npos,
          "structured event names reach the file");
    check(content.find("binding_missing") != std::string::npos,
          "structured warning names reach the file");
    check(content.find("request_id") != std::string::npos,
          "structured fields reach the file");
}

void disabled_logger_never_claims_durability() {
    Logger logger(LoggerConfig{});
    logger.write(Severity::kDebug, "debug_only");
    const auto snapshot = logger.snapshot();
    check(!snapshot.at("enabled").get<bool>(), "an empty path disables file logging");
    check(snapshot.at("accepted") == 0, "disabled diagnostics are not counted as durable");
    check(snapshot.at("written") == 0, "disabled diagnostics are not counted as written");
    logger.shutdown();
}

}  /* namespace */

int main() {
    invalid_logger_configuration_and_events_are_rejected();
    accepted_records_are_structured_and_drained_on_shutdown();
    disabled_logger_never_claims_durability();
    return context_hmi_test::report("diagnostic logger tests");
}
