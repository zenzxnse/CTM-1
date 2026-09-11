#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace context_hmi::diagnostics {

enum class Severity { kDebug, kInfo, kWarning, kError };

struct LoggerConfig {
  std::filesystem::path path;
  std::size_t queue_capacity{1024};
  std::uintmax_t rotation_bytes{8U * 1024U * 1024U};
  std::size_t rotation_files{3};
  std::size_t maximum_record_bytes{1024U * 1024U};
};

/** Bounded structured diagnostic logger with visible overflow and rotation. */
class Logger {
 public:
  explicit Logger(LoggerConfig config);
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  ~Logger();

  void write(Severity severity, std::string event,
             nlohmann::json fields = nlohmann::json::object());
  void shutdown();
  nlohmann::json snapshot() const;

 private:
  void run();
  void write_record(const std::string& record);
  void rotate_if_needed(std::size_t incoming_bytes);

  LoggerConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> queue_;
  std::thread worker_;
  bool accepting_{true};
  bool stopping_{false};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> fallback_{0};
  std::atomic<bool> failed_{false};
};

std::string severity_name(Severity severity);

}  /* namespace context_hmi::diagnostics */
