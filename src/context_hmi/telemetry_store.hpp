#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace context_hmi::telemetry {

using Json = nlohmann::json;

/** Holds bounded current values supplied by an external data adapter. */
class Store {
 public:
  explicit Store(std::uint64_t stale_after_ms = 3000,
                 std::size_t maximum_values_per_batch = 512);

  Json ingest(const Json& model, std::uint64_t context_generation,
              const Json& batch);
  Json snapshot(const Json& model, std::uint64_t context_generation,
                const std::string& session_id) const;
  void reconfigure(std::uint64_t context_generation);
  Json readiness(std::uint64_t context_generation) const;
  Json metrics() const;

 private:
  static std::uint64_t unix_time_ms();

  const std::uint64_t stale_after_ms_;
  const std::size_t maximum_values_per_batch_;
  mutable std::mutex mutex_;
  Json values_ = Json::object();
  std::unordered_map<std::string, std::uint64_t> source_sequences_;
  std::uint64_t context_generation_{0};
  std::uint64_t accepted_batches_{0};
  std::uint64_t rejected_batches_{0};
  std::uint64_t accepted_values_{0};
  std::uint64_t last_received_ms_{0};
};

}  /* namespace context_hmi::telemetry */
