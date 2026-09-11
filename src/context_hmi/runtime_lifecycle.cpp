#include "context_hmi/service_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "context_hmi/engine.hpp"

namespace context_hmi::service {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaximumModelBytes = 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = 8192;

std::size_t native_worker_count() {
  const auto available = std::max(1U, std::thread::hardware_concurrency());
#if defined(CONTEXT_HMI_CONSTRAINED)
  return std::min<std::size_t>(2, available);
#else
  return std::min<std::size_t>(4, available);
#endif
}

std::size_t provider_worker_count() {
#if defined(CONTEXT_HMI_CONSTRAINED)
  return 1;
#else
  return 4;
#endif
}

bool is_ascii_alphanumeric(unsigned char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9');
}

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 128 ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(id.front()))) {
    return false;
  }
  return std::all_of(id.begin(), id.end(), [](unsigned char character) {
    return is_ascii_alphanumeric(character) || character == '_' || character == '.' ||
           character == '-';
  });
}

enum class JsonShape { kOk, kTooDeep, kIncomplete };

JsonShape inspect_json_shape(const std::string& input) {
  std::size_t depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char character : input) {
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        quoted = false;
      }
      continue;
    }
    if (character == '"') {
      quoted = true;
    } else if (character == '{' || character == '[') {
      if (++depth > 32) {
        return JsonShape::kTooDeep;
      }
    } else if (character == '}' || character == ']') {
      if (depth == 0) {
        return JsonShape::kIncomplete;
      }
      --depth;
    }
  }
  return !quoted && depth == 0 ? JsonShape::kOk : JsonShape::kIncomplete;
}

std::string read_file_bounded(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
    throw std::runtime_error("model file is missing or not a regular file");
  }
  const auto size = fs::file_size(path, error);
  if (error || size > kMaximumModelBytes) {
    throw std::runtime_error("model file is too large");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open model file");
  }
  std::string body((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  if (body.size() > kMaximumModelBytes) {
    throw std::runtime_error("model file is too large");
  }
  return body;
}

}  /* namespace */

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)),
      cpu_workers_(native_worker_count(),
#if defined(CONTEXT_HMI_CONSTRAINED)
                   16),
#else
                   64),
#endif
      provider_workers_(provider_worker_count(), 16),
      service_lifecycle_("service", lifecycle::State::kStarting),
      source_lifecycle_("source", lifecycle::State::kStarting),
      provider_lifecycle_("provider", lifecycle::State::kStarting),
      store_lifecycle_("store", lifecycle::State::kDisabled),
      inference_(InferenceConfig{options_.provider_url,
                                 std::chrono::milliseconds(options_.provider_timeout_ms),
                                 kMaximumPromptBytes,
                                 64U * 1024U,
                                 static_cast<std::size_t>(options_.provider_context_bytes),
                                 options_.provider_max_output_tokens,
                                 options_.provider_model,
                                 options_.provider_api_key,
                                 options_.allow_remote_inference}),
      retrieval_cache_(cache::Config{
          static_cast<std::size_t>(options_.retrieval_cache_capacity),
          std::chrono::milliseconds(options_.cache_ttl_ms)}),
      interpretation_cache_(cache::Config{
          static_cast<std::size_t>(options_.interpretation_cache_capacity),
          std::chrono::milliseconds(options_.cache_ttl_ms)}),
      diagnostic_logger_(diagnostics::LoggerConfig{
          .path = options_.diagnostic_log_file,
          .queue_capacity =
              static_cast<std::size_t>(options_.diagnostic_queue_capacity),
          .rotation_bytes = options_.diagnostic_rotation_bytes,
          .rotation_files =
              static_cast<std::size_t>(options_.diagnostic_rotation_files)}),
      telemetry_store_(options_.telemetry_stale_after_ms,
                       static_cast<std::size_t>(options_.telemetry_max_values)),
      active_scenario_(fs::path(options_.model).stem().string()),
      active_model_path_(options_.model),
      session_id_(make_session_id()) {}

Runtime::~Runtime() { shutdown(); }

void Runtime::start() {
  if (started_.exchange(true, std::memory_order_acq_rel)) {
    throw std::logic_error("runtime was already started");
  }
  model_ = load_model_file(options_.model);
  telemetry_store_.reconfigure(0);
  if (!options_.store_file.empty()) {
    store_lifecycle_.transition(lifecycle::State::kStarting);
    try {
      journal_ = std::make_unique<storage::Journal>(storage::JournalConfig{
          .path = options_.store_file,
          .maximum_payload_bytes = 64U * 1024U,
          .maximum_file_bytes = options_.store_max_bytes,
          .recent_record_limit = 512,
          .synchronize = options_.store_sync});
      restore_journal_records();
      store_lifecycle_.transition(lifecycle::State::kReady);
    } catch (const std::exception& error) {
      store_lifecycle_.transition(lifecycle::State::kFailed, error.what());
      throw;
    }
  }
  source_lifecycle_.transition(lifecycle::State::kDegraded,
                               "waiting for current telemetry");
  if (options_.provider_url.empty()) {
    provider_lifecycle_.transition(lifecycle::State::kReady,
                                   "deterministic rules provider");
  }
  service_lifecycle_.transition(lifecycle::State::kReady);
  diagnostic_logger_.write(
      diagnostics::Severity::kInfo, "service_started",
      Json{{"session_id", session_id_},
           {"model_id", model_.at("model_id")},
           {"model_revision", model_.at("revision")},
           {"source", "telemetry-ingestion"},
           {"provider", inference_.mode()}});
  if (journal_) {
    append_operational("service_start",
                       Json{{"session_id", session_id_},
                            {"model_id", model_.at("model_id")},
                            {"revision", model_.at("revision")},
                            {"timestamp_ms", unix_time_ms()}});
  }
  if (options_.watch_model) {
    model_watcher_ = std::jthread([this](std::stop_token stop) { watch_models(stop); });
  }
}

void Runtime::shutdown() {
  if (!started_.load(std::memory_order_acquire) ||
      stopped_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  service_lifecycle_.transition(lifecycle::State::kStopping);
  source_lifecycle_.transition(lifecycle::State::kStopping);
  provider_lifecycle_.transition(lifecycle::State::kStopping);
  if (store_lifecycle_.state() != lifecycle::State::kDisabled &&
      store_lifecycle_.state() != lifecycle::State::kFailed) {
    store_lifecycle_.transition(lifecycle::State::kStopping);
  }
  model_watcher_.request_stop();
  watcher_condition_.notify_all();
  if (model_watcher_.joinable()) {
    model_watcher_.join();
  }
  provider_workers_.shutdown();
  cpu_workers_.shutdown();
  diagnostic_logger_.write(diagnostics::Severity::kInfo, "service_stopped",
                           Json{{"session_id", session_id_}});
  diagnostic_logger_.shutdown();
  service_lifecycle_.transition(lifecycle::State::kStopped);
  source_lifecycle_.transition(lifecycle::State::kStopped);
  provider_lifecycle_.transition(lifecycle::State::kStopped);
  if (store_lifecycle_.state() == lifecycle::State::kStopping) {
    store_lifecycle_.transition(lifecycle::State::kStopped);
  }
}

Json Runtime::load_model_file(const std::string& path) {
  try {
    const auto body = read_file_bounded(path);
    const auto shape = inspect_json_shape(body);
    if (shape == JsonShape::kTooDeep) {
      throw std::runtime_error("model JSON exceeds nesting limit");
    }
    if (shape == JsonShape::kIncomplete) {
      throw std::runtime_error("model JSON is incomplete");
    }
    auto model = Json::parse(body);
    if (model.is_object() && model.contains("model")) {
      for (const auto& field : model.items()) {
        if (field.key() != "model" && field.key() != "report") {
          throw std::runtime_error(
              "engineering import output contains an unsupported field");
        }
      }
      if (!model.at("model").is_object() ||
          (model.contains("report") && !model.at("report").is_object())) {
        throw std::runtime_error("engineering import output has an invalid shape");
      }
      model = model.at("model");
    }
    validate_model(model);
    return model;
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("invalid model: ") + error.what());
  }
}

std::string Runtime::make_session_id() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  return "session-" + std::to_string(now) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

std::uint64_t Runtime::unix_time_ms() {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return static_cast<std::uint64_t>(elapsed.count());
}

std::pair<Json, std::uint64_t> Runtime::model_snapshot() const {
  std::lock_guard<std::mutex> lock(model_mutex_);
  return {model_, model_generation_.load(std::memory_order_relaxed)};
}

Json Runtime::public_model() const {
  auto [model, generation] = model_snapshot();
  model["session_id"] = session_id_;
  model["context_generation"] = generation;
  return model;
}

Json Runtime::scenarios() const {
  Json result = Json::array();
  std::error_code error;
  if (!fs::is_directory(options_.models_dir, error)) {
    return Json{{"scenarios", std::move(result)}};
  }
  for (const auto& entry : fs::directory_iterator(options_.models_dir, error)) {
    if (error || entry.is_symlink(error) || !entry.is_regular_file(error) ||
        entry.path().extension() != ".json") {
      continue;
    }
    try {
      auto candidate = load_model_file(entry.path().string());
      const auto id = entry.path().stem().string();
      if (!valid_id(id)) {
        continue;
      }
      const auto name = candidate.value(
          "scenario_name",
          candidate.value("name", candidate.at("model_id").get<std::string>()));
      result.push_back(Json{{"id", id},
                            {"model_id", candidate.at("model_id")},
                            {"revision", candidate.at("revision")},
                            {"name", name},
                            {"description", candidate.value("description", "")}});
    } catch (...) {
      /** Invalid files are excluded from selectable scenarios. */
    }
  }
  std::sort(result.begin(), result.end(), [](const Json& left, const Json& right) {
    return left.value("id", "") < right.value("id", "");
  });
  return Json{{"scenarios", std::move(result)}};
}

Json Runtime::select_scenario(const std::string& id) {
  if (!valid_id(id)) {
    throw DomainError{"invalid_scenario", "scenario id is invalid"};
  }
  const fs::path scenario = fs::path(options_.models_dir) / (id + ".json");
  std::error_code error;
  if (fs::is_symlink(fs::symlink_status(scenario, error))) {
    throw DomainError{"scenario_not_found", "scenario symlinks are not permitted"};
  }
  Json candidate;
  try {
    candidate = load_model_file(scenario.string());
  } catch (const std::exception& failure) {
    throw DomainError{"scenario_not_found", failure.what()};
  }
  return apply_model_change(std::move(candidate), id, scenario.string(), "scenario-selected");
}

Json Runtime::apply_model_change(Json candidate, std::string scenario,
                                 std::string source_path, const std::string& cause) {
  std::lock_guard<std::mutex> change_lock(model_change_mutex_);
  std::uint64_t generation = 0;
  std::size_t reconciled = 0;
  std::size_t review_required = 0;
  {
    std::lock_guard<std::mutex> model_lock(model_mutex_);
    if (cause == "file-rescan" &&
        candidate.at("model_id") == model_.at("model_id") &&
        candidate.at("revision").get<std::uint64_t>() <=
            model_.at("revision").get<std::uint64_t>()) {
      throw DomainError{"stale_model_revision",
                        "changed model content must advance its declared revision"};
    }
    generation = model_generation_.load(std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> view_lock(view_mutex_);
    for (auto& [id, view] : views_) {
      try {
        auto revised = reconcile_view(candidate, view);
        revised["view_id"] = id;
        revised["session_id"] = session_id_;
        revised["context_generation"] = generation;
        view = std::move(revised);
        ++reconciled;
      } catch (const DomainError& error) {
        view["status"] = "needs-review";
        if (!view.contains("issues") || !view.at("issues").is_array()) {
          view["issues"] = Json::array();
        }
        view["issues"].push_back(
            Json{{"code", "model-replacement-requires-new-request"},
                 {"severity", "error"},
                 {"message", error.details}});
        view["context_generation"] = generation;
        ++review_required;
      }
    }
    model_ = candidate;
    active_scenario_ = std::move(scenario);
    active_model_path_ = std::move(source_path);
    model_generation_.store(generation, std::memory_order_release);
  }
  model_change_count_.fetch_add(1, std::memory_order_relaxed);
  retrieval_cache_.clear();
  interpretation_cache_.clear();
  telemetry_store_.reconfigure(generation);
  Json record{{"cause", cause},
              {"scenario", active_scenario_},
              {"model_id", candidate.at("model_id")},
              {"revision", candidate.at("revision")},
              {"context_generation", generation},
              {"reconciled_views", reconciled},
              {"review_required_views", review_required},
              {"timestamp_ms", unix_time_ms()}};
  try {
    append_operational("model_selected", record);
  } catch (const std::exception& error) {
    diagnostic_logger_.write(diagnostics::Severity::kError, "model_record_failed",
                             Json{{"reason", error.what()}});
  }
  diagnostic_logger_.write(diagnostics::Severity::kInfo, "model_selected", record);
  return public_model();
}

void Runtime::watch_models(std::stop_token stop) {
  std::string observed_path;
  fs::file_time_type observed_time{};
  bool have_time = false;
  while (!stop.stop_requested()) {
    std::string path;
    {
      std::lock_guard<std::mutex> lock(model_mutex_);
      path = active_model_path_;
    }
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    const bool safe = !error && !fs::is_symlink(status) && fs::is_regular_file(status);
    const auto modified = safe ? fs::last_write_time(path, error) : fs::file_time_type{};
    if (path != observed_path) {
      observed_path = path;
      observed_time = modified;
      have_time = safe && !error;
    } else if (safe && !error && have_time && modified != observed_time) {
      observed_time = modified;
      try {
        auto candidate = load_model_file(path);
        const auto scenario = fs::path(path).stem().string();
        if (candidate != model_snapshot().first) {
          static_cast<void>(
              apply_model_change(std::move(candidate), scenario, path, "file-rescan"));
        }
      } catch (const std::exception& failure) {
        model_watch_failures_.fetch_add(1, std::memory_order_relaxed);
        diagnostic_logger_.write(diagnostics::Severity::kError, "model_rescan_rejected",
                                 Json{{"path", path}, {"reason", failure.what()}});
      }
    } else if (!safe || error) {
      model_watch_failures_.fetch_add(1, std::memory_order_relaxed);
      have_time = false;
    } else if (!have_time) {
      observed_time = modified;
      have_time = true;
    }
    std::unique_lock<std::mutex> lock(watcher_mutex_);
    watcher_condition_.wait_for(lock, std::chrono::milliseconds(options_.model_rescan_ms),
                                [&stop] { return stop.stop_requested(); });
  }
}

}  /* namespace context_hmi::service */
