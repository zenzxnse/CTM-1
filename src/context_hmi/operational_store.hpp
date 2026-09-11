#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace context_hmi::storage {

struct JournalConfig {
  std::filesystem::path path;
  std::size_t maximum_payload_bytes{64U * 1024U};
  std::uintmax_t maximum_file_bytes{64U * 1024U * 1024U};
  std::size_t recent_record_limit{256};
  bool synchronize{true};
};

struct JournalRecord {
  std::uint64_t sequence;
  std::string kind;
  nlohmann::json payload;
};

/** Append-only framed operational journal with bounded replay and tail recovery. */
class Journal {
 public:
  explicit Journal(JournalConfig config);
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  std::uint64_t append(const std::string& kind, const nlohmann::json& payload);
  std::vector<JournalRecord> recent(std::size_t limit) const;
  nlohmann::json snapshot() const;

 private:
  void open_and_recover();

  JournalConfig config_;
  mutable std::mutex mutex_;
  std::uint64_t next_sequence_{1};
  std::uintmax_t recovered_tail_bytes_{0};
  std::vector<JournalRecord> recent_;
};

}  /* namespace context_hmi::storage */

