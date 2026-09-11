#include "context_hmi/diagnostic_logger.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace context_hmi::diagnostics {
namespace {

std::uint64_t unix_time_ms() {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return static_cast<std::uint64_t>(elapsed.count());
}

bool low_severity(Severity severity) {
  return severity == Severity::kDebug || severity == Severity::kInfo;
}

}  /* namespace */

Logger::Logger(LoggerConfig config) : config_(std::move(config)) {
  if (config_.queue_capacity == 0 || config_.rotation_bytes < 4096 ||
      config_.rotation_files == 0 || config_.rotation_files > 32 ||
      config_.maximum_record_bytes < 256) {
    throw std::invalid_argument("diagnostic logger configuration is invalid");
  }
  if (!config_.path.empty()) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(config_.path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("could not inspect diagnostic log path");
    }
    if (!error && std::filesystem::is_symlink(status)) {
      throw std::runtime_error("diagnostic log must not be a symbolic link");
    }
    if (!error && std::filesystem::exists(status) &&
        !std::filesystem::is_regular_file(status)) {
      throw std::runtime_error("diagnostic log path is not a regular file");
    }
    const auto parent = config_.path.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
      throw std::runtime_error("diagnostic log parent directory does not exist");
    }
    worker_ = std::thread([this] { run(); });
  }
}

Logger::~Logger() { shutdown(); }

void Logger::write(Severity severity, std::string event, nlohmann::json fields) {
  if (event.empty() || event.size() > 128 || !fields.is_object()) {
    throw std::invalid_argument("diagnostic event is invalid");
  }
  nlohmann::json record{{"timestamp_ms", unix_time_ms()},
                        {"severity", severity_name(severity)},
                        {"event", std::move(event)},
                        {"fields", std::move(fields)}};
  std::string encoded = record.dump() + '\n';
  if (encoded.size() > config_.maximum_record_bytes ||
      encoded.size() > config_.rotation_bytes) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    if (!low_severity(severity)) {
      std::cerr << nlohmann::json{
                           {"timestamp_ms", unix_time_ms()},
                           {"severity", "error"},
                           {"event", "diagnostic_record_rejected"},
                           {"fields", nlohmann::json{{"reason", "record_too_large"},
                                                      {"bytes", encoded.size()}}}}
                           .dump()
                << '\n';
      fallback_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (config_.path.empty()) {
    if (!low_severity(severity)) {
      std::cerr << encoded;
      fallback_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || queue_.size() >= config_.queue_capacity) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      if (!low_severity(severity)) {
        std::cerr << encoded;
        fallback_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    queue_.push_back(std::move(encoded));
  }
  accepted_.fetch_add(1, std::memory_order_relaxed);
  condition_.notify_one();
}

void Logger::run() {
  for (;;) {
    std::string record;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) {
        return;
      }
      record = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      write_record(record);
      written_.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception& error) {
      failed_.store(true, std::memory_order_release);
      fallback_.fetch_add(1, std::memory_order_relaxed);
      std::cerr << record;
      std::cerr << nlohmann::json{{"timestamp_ms", unix_time_ms()},
                                  {"severity", "error"},
                                  {"event", "diagnostic_write_failed"},
                                  {"fields", nlohmann::json{{"reason", error.what()}}}}
                       .dump()
                << '\n';
    }
  }
}

void Logger::write_record(const std::string& record) {
  rotate_if_needed(record.size());
  std::error_code error;
  const auto status = std::filesystem::symlink_status(config_.path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("could not inspect diagnostic log");
  }
  if (!error && (std::filesystem::is_symlink(status) ||
                 (std::filesystem::exists(status) &&
                  !std::filesystem::is_regular_file(status)))) {
    throw std::runtime_error("diagnostic log path is not a regular file");
  }
  std::ofstream output(config_.path, std::ios::binary | std::ios::app);
  if (!output) {
    throw std::runtime_error("could not open diagnostic log");
  }
  output.write(record.data(), static_cast<std::streamsize>(record.size()));
  output.flush();
  if (!output) {
    throw std::runtime_error("diagnostic log write failed");
  }
}

void Logger::rotate_if_needed(std::size_t incoming_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(config_.path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("could not inspect diagnostic log for rotation");
  }
  if (!error && (std::filesystem::is_symlink(status) ||
                 (std::filesystem::exists(status) &&
                  !std::filesystem::is_regular_file(status)))) {
    throw std::runtime_error("diagnostic log path is not a regular file");
  }
  if (!std::filesystem::exists(config_.path, error)) {
    return;
  }
  const auto size = std::filesystem::file_size(config_.path, error);
  if (error) {
    throw std::runtime_error("could not inspect diagnostic log for rotation");
  }
  if (size <= config_.rotation_bytes &&
      incoming_bytes <= config_.rotation_bytes - size) {
    return;
  }
  for (std::size_t index = config_.rotation_files; index > 1; --index) {
    const auto older = config_.path.string() + "." + std::to_string(index - 1);
    const auto newer = config_.path.string() + "." + std::to_string(index);
    if (std::filesystem::exists(older, error)) {
      std::filesystem::remove(newer, error);
      error.clear();
      std::filesystem::rename(older, newer, error);
      if (error) {
        throw std::runtime_error("diagnostic log rotation failed");
      }
    }
  }
  const auto first = config_.path.string() + ".1";
  std::filesystem::remove(first, error);
  error.clear();
  std::filesystem::rename(config_.path, first, error);
  if (error) {
    throw std::runtime_error("diagnostic log rotation failed");
  }
}

void Logger::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    accepting_ = false;
    stopping_ = true;
  }
  condition_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

nlohmann::json Logger::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nlohmann::json{{"enabled", !config_.path.empty()},
                        {"queue_depth", queue_.size()},
                        {"queue_capacity", config_.queue_capacity},
                        {"accepting", accepting_},
                        {"accepted", accepted_.load(std::memory_order_relaxed)},
                        {"written", written_.load(std::memory_order_relaxed)},
                        {"dropped", dropped_.load(std::memory_order_relaxed)},
                        {"fallback", fallback_.load(std::memory_order_relaxed)},
                        {"failed", failed_.load(std::memory_order_relaxed)},
                        {"rotation_bytes", config_.rotation_bytes},
                        {"rotation_files", config_.rotation_files},
                        {"maximum_record_bytes", config_.maximum_record_bytes}};
}

std::string severity_name(Severity severity) {
  switch (severity) {
    case Severity::kDebug:
      return "debug";
    case Severity::kInfo:
      return "info";
    case Severity::kWarning:
      return "warning";
    case Severity::kError:
      return "error";
  }
  throw std::logic_error("unknown diagnostic severity");
}

}  /* namespace context_hmi::diagnostics */
