#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace context_hmi::cache {

using Json = nlohmann::json;

struct Config {
  std::size_t capacity{128};
  std::chrono::milliseconds time_to_live{300000};
};

/** A bounded, expiring LRU cache for immutable JSON computation results. */
class BoundedJsonCache final {
 public:
  explicit BoundedJsonCache(Config config);
  BoundedJsonCache(const BoundedJsonCache&) = delete;
  BoundedJsonCache& operator=(const BoundedJsonCache&) = delete;

  std::optional<Json> get(const std::string& key);
  void put(std::string key, Json value);
  void clear();
  Json snapshot() const;

 private:
  using Clock = std::chrono::steady_clock;

  struct Entry {
    Json value;
    Clock::time_point expires_at;
    std::list<std::string>::iterator recency;
  };

  void erase_locked(
      std::unordered_map<std::string, Entry>::iterator location);

  Config config_;
  mutable std::mutex mutex_;
  std::list<std::string> recency_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t hits_{0};
  std::uint64_t misses_{0};
  std::uint64_t expirations_{0};
  std::uint64_t evictions_{0};
  std::uint64_t invalidations_{0};
};

}  /* namespace context_hmi::cache */
