#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "context_hmi/inference.hpp"
#include "context_hmi/bounded_cache.hpp"
#include "context_hmi/diagnostic_logger.hpp"
#include "context_hmi/lifecycle.hpp"
#include "context_hmi/operational_store.hpp"
#include "context_hmi/options.hpp"
#include "context_hmi/telemetry_store.hpp"
#include "context_hmi/worker_pool.hpp"

namespace context_hmi::service {

using Json = nlohmann::json;

/** Owns validated runtime state independently of the Drogon transport. */
class Runtime {
 public:
  explicit Runtime(RuntimeOptions options);
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  ~Runtime();

  void start();
  void shutdown();

  const RuntimeOptions& options() const { return options_; }
  execution::WorkerPool& cpu_workers() { return cpu_workers_; }
  execution::WorkerPool& provider_workers() { return provider_workers_; }
  std::string inference_mode() const { return inference_.mode(); }

  Json health() const;
  Json readiness();
  Json session() const;
  Json metrics() const;
  Json public_model() const;
  Json catalog() const;
  Json scenarios() const;
  Json telemetry() const;
  Json ingest_telemetry(const Json& batch);
  Json retrieve(const std::string& prompt);
  Json interpret(const std::string& prompt, const std::string& mode,
                 const Json& client = Json::object());
  Json resolve(const Json& task);
  Json reconcile(const Json& supplied_view);
  Json select_scenario(const std::string& id);
  Json audit(std::size_t limit) const;
  Json records(std::size_t limit) const;

  bool acquire_sse_client();
  void release_sse_client();
  std::size_t maximum_sse_clients() const;
  bool acquire_inference();
  void release_inference();

 private:
  static Json load_model_file(const std::string& path);
  static std::string make_session_id();
  static std::uint64_t unix_time_ms();
  static constexpr std::size_t maximum_saved_views() {
#if defined(CONTEXT_HMI_CONSTRAINED)
    return 24;
#else
    return 64;
#endif
  }

  std::pair<Json, std::uint64_t> model_snapshot() const;
  std::optional<std::string> publish_view(Json& view, std::uint64_t generation);
  std::optional<Json> trusted_view(const Json& supplied) const;
  void restore_journal_records();
  void append_operational(const std::string& kind, const Json& record);
  Json apply_model_change(Json candidate, std::string scenario,
                          std::string source_path, const std::string& cause);
  void watch_models(std::stop_token stop);

  RuntimeOptions options_;
  execution::WorkerPool cpu_workers_;
  execution::WorkerPool provider_workers_;
  lifecycle::Machine service_lifecycle_;
  lifecycle::Machine source_lifecycle_;
  lifecycle::Machine provider_lifecycle_;
  lifecycle::Machine store_lifecycle_;
  Inference inference_;
  cache::BoundedJsonCache retrieval_cache_;
  cache::BoundedJsonCache interpretation_cache_;
  diagnostics::Logger diagnostic_logger_;
  telemetry::Store telemetry_store_;
  std::unique_ptr<storage::Journal> journal_;

  mutable std::mutex model_mutex_;
  Json model_;
  std::string active_scenario_;
  std::string active_model_path_;
  std::atomic<std::uint64_t> model_generation_{0};
  std::atomic<std::uint64_t> model_change_count_{0};
  std::atomic<std::uint64_t> model_watch_failures_{0};
  mutable std::mutex model_change_mutex_;

  mutable std::mutex view_mutex_;
  std::unordered_map<std::string, Json> views_;
  std::deque<std::string> view_order_;
  std::atomic<std::uint64_t> view_sequence_{0};

  std::atomic<unsigned> inference_inflight_{0};
  std::atomic<unsigned> sse_clients_{0};
  std::string session_id_;

  mutable std::mutex watcher_mutex_;
  std::condition_variable watcher_condition_;
  std::jthread model_watcher_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
};

}  /* namespace context_hmi::service */
