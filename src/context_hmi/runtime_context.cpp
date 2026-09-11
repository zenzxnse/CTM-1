#include "context_hmi/service_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "context_hmi/context_retrieval.hpp"
#include "context_hmi/engine.hpp"

namespace context_hmi::service {
namespace {

std::string normalized_cache_prompt(const std::string& prompt) {
  std::string result;
  result.reserve(prompt.size());
  bool pending_space = false;
  for (const unsigned char character : prompt) {
    if (std::isspace(character) != 0) {
      pending_space = !result.empty();
      continue;
    }
    if (pending_space) {
      result.push_back(' ');
      pending_space = false;
    }
    result.push_back(character >= 'A' && character <= 'Z'
                         ? static_cast<char>(character - 'A' + 'a')
                         : static_cast<char>(character));
  }
  return result;
}

context::RetrievalLimits retrieval_limits(const RuntimeOptions& options) {
  context::RetrievalLimits limits;
  limits.max_serialized_bytes = static_cast<std::size_t>(options.provider_context_bytes);
  limits.max_estimated_tokens = (limits.max_serialized_bytes + 3U) / 4U;
  return limits;
}

std::string cache_key(const Json& model, std::uint64_t generation,
                      const RuntimeOptions& options, const std::string& mode,
                      const std::string& prompt) {
  return model.at("model_id").get<std::string>() + "\x1f" +
         std::to_string(model.at("revision").get<std::uint64_t>()) + "\x1f" +
         std::to_string(generation) + "\x1f" + mode + "\x1f" +
         options.provider_url + "\x1f" + options.provider_model + "\x1f" +
         std::to_string(options.provider_timeout_ms) + "\x1f" +
         std::to_string(options.provider_context_bytes) + "\x1f" +
         std::to_string(options.provider_max_output_tokens) + "\x1f" +
         std::to_string(options.allow_remote_inference) + "\x1f" +
         normalized_cache_prompt(prompt);
}

Json context_summary(const Json& retrieval) {
  const auto serialized_bytes = retrieval.dump().size();
  const auto& scope = retrieval.at("scope");
  const auto& truncated = retrieval.at("truncated");
  bool any_truncated = false;
  for (const auto& item : truncated.items()) {
    any_truncated = any_truncated || item.value().get<bool>();
  }
  return Json{{"serialized_bytes", serialized_bytes},
              {"bytes", serialized_bytes},
              {"estimated_tokens", (serialized_bytes + 3U) / 4U},
              {"token_estimate", "utf8-bytes-divided-by-four"},
              {"candidate_count", retrieval.at("candidates").size()},
              {"candidates", retrieval.at("candidates").size()},
              {"asset_count", scope.at("assets").size()},
              {"assets", scope.at("assets").size()},
              {"relationship_count", scope.at("relationships").size()},
              {"relationships", scope.at("relationships").size()},
              {"tag_count", scope.at("tags").size()},
              {"tags", scope.at("tags").size()},
              {"alarm_count", scope.at("alarms").size()},
              {"alarms", scope.at("alarms").size()},
              {"truncation", truncated},
              {"truncated", any_truncated},
              {"any_truncated", any_truncated}};
}

}  /* namespace */

Json Runtime::retrieve(const std::string& prompt) {
  auto [model, generation] = model_snapshot();
  const auto limits = retrieval_limits(options_);
  const auto key = cache_key(model, generation, options_, "retrieval", prompt);
  const auto started = std::chrono::steady_clock::now();
  auto cached = retrieval_cache_.get(key);
  const bool cache_hit = cached.has_value();
  auto result = cached ? std::move(*cached) : context::retrieve(model, prompt, limits);
  if (!cache_hit) {
    retrieval_cache_.put(key, result);
  }
  result["session_id"] = session_id_;
  result["context_generation"] = generation;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  result["execution"] =
      Json{{"cache", Json{{"retrieval", Json{{"state", cache_hit ? "hit" : "miss"}}}}},
           {"context", context_summary(result)},
           {"elapsed_us", elapsed.count()},
           {"elapsed_ms", static_cast<double>(elapsed.count()) / 1000.0},
           {"model_id", model.at("model_id")},
           {"model_revision", model.at("revision")},
           {"context_generation", generation}};
  diagnostic_logger_.write(
      diagnostics::Severity::kDebug, "context_retrieved",
      Json{{"model_id", model.at("model_id")},
           {"model_revision", model.at("revision")},
           {"context_generation", generation},
           {"cache_hit", cache_hit},
           {"context", result.at("execution").at("context")},
           {"elapsed_us", elapsed.count()}});
  return result;
}

Json Runtime::interpret(const std::string& prompt, const std::string& mode,
                        const Json& client) {
  auto [model, generation] = model_snapshot();
  const auto started = std::chrono::steady_clock::now();
  const auto limits = retrieval_limits(options_);
  const auto retrieval_key = cache_key(model, generation, options_, "retrieval", prompt);
  auto cached_retrieval = retrieval_cache_.get(retrieval_key);
  const bool retrieval_hit = cached_retrieval.has_value();
  Json retrieval = cached_retrieval ? std::move(*cached_retrieval)
                                    : context::retrieve(model, prompt, limits);
  if (!retrieval_hit) {
    retrieval_cache_.put(retrieval_key, retrieval);
  }
  const std::string selected_mode = mode.empty() ? inference_.mode() : mode;
  const auto interpretation_key =
      cache_key(model, generation, options_, selected_mode, prompt);
  auto cached_interpretation = interpretation_cache_.get(interpretation_key);
  const bool interpretation_hit = cached_interpretation.has_value();
  Json result;
  Json provider_usage{{"reported", false},
                      {"provider_call", false}};
  try {
    result = cached_interpretation
                 ? std::move(*cached_interpretation)
                 : inference_.interpret(model, prompt, retrieval, mode);
  } catch (const std::exception& error) {
    diagnostic_logger_.write(
        diagnostics::Severity::kWarning, "interpretation_failed",
        Json{{"model_id", model.at("model_id")},
             {"model_revision", model.at("revision")},
             {"context_generation", generation},
             {"interpreter", selected_mode},
             {"reason", error.what()}});
    throw;
  }
  if (!interpretation_hit) {
    provider_usage = result.value("provider_usage", provider_usage);
    provider_usage["provider_call"] = selected_mode != "rules";
    result.erase("provider_usage");
    result.erase("retrieval");
    interpretation_cache_.put(interpretation_key, result);
  }
  if (result.value("status", "") == "ready" && result.contains("task")) {
    result["task"]["original_request"] = prompt;
    if (!client.empty()) {
      result["task"]["client"] = client;
    }
  }
  if (generation != model_generation_.load(std::memory_order_acquire)) {
    throw InferenceError("stale_context",
                         "active model changed while inference was running");
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  result["execution"] =
      Json{{"cache", Json{{"retrieval", Json{{"state", retrieval_hit ? "hit" : "miss"}}},
                           {"interpretation",
                            Json{{"state", interpretation_hit ? "hit" : "miss"}}}}},
           {"context", context_summary(retrieval)},
           {"elapsed_us", elapsed.count()},
           {"elapsed_ms", static_cast<double>(elapsed.count()) / 1000.0},
           {"provider_call", !interpretation_hit && selected_mode != "rules"},
           {"provider_usage", provider_usage},
           {"model_id", model.at("model_id")},
           {"model_revision", model.at("revision")},
           {"context_generation", generation},
           {"validation", "native-scope-validated"}};
  result["provider_usage"] = provider_usage;
  diagnostic_logger_.write(
      diagnostics::Severity::kInfo, "interpretation_completed",
      Json{{"model_id", model.at("model_id")},
           {"model_revision", model.at("revision")},
           {"context_generation", generation},
           {"interpreter", result.value("interpreter", selected_mode)},
           {"status", result.value("status", "unknown")},
           {"retrieval_cache_hit", retrieval_hit},
           {"interpretation_cache_hit", interpretation_hit},
           {"elapsed_us", elapsed.count()}});
  if (result.value("status", "") != "ready") {
    return result;
  }
  if (!result.contains("task") || !result.at("task").is_object()) {
    throw InferenceError("provider_schema", "ready interpretation requires task");
  }
  auto& task = result.at("task");
  if (task.contains("model_id") && task.at("model_id") != model.at("model_id")) {
    throw InferenceError("provider_schema",
                         "provider task model_id does not match active model");
  }
  if (task.contains("model_revision") &&
      task.at("model_revision") != model.at("revision")) {
    throw InferenceError("provider_schema",
                         "provider task model_revision does not match active model");
  }
  if (task.contains("anchor_asset_id")) {
    const auto selected = task.at("anchor_asset_id").get<std::string>();
    const bool known = std::any_of(
        model.at("assets").begin(), model.at("assets").end(),
        [&](const Json& asset) { return asset.value("id", "") == selected; });
    if (!known) {
      throw InferenceError(
          "provider_schema",
          "provider task anchor_asset_id is not in the active model");
    }
  }
  task["model_id"] = model.at("model_id");
  task["model_revision"] = model.at("revision");
  auto view = resolve_task(model, task);
  if (!publish_view(view, generation)) {
    throw InferenceError("stale_context",
                         "active model changed while publishing view");
  }
  result["view"] = std::move(view);
  return result;
}

Json Runtime::resolve(const Json& task) {
  auto [model, generation] = model_snapshot();
  auto view = resolve_task(model, task);
  if (!publish_view(view, generation)) {
    throw DomainError{"stale_context", "active model changed while publishing view"};
  }
  return view;
}

std::optional<std::string> Runtime::publish_view(Json& view, std::uint64_t generation) {
  const auto id = session_id_ + "-view-" +
                  std::to_string(view_sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
  {
    std::lock_guard<std::mutex> model_lock(model_mutex_);
    if (model_generation_.load(std::memory_order_relaxed) != generation) {
      return std::nullopt;
    }
    view["view_id"] = id;
    view["session_id"] = session_id_;
    view["context_generation"] = generation;
    std::lock_guard<std::mutex> view_lock(view_mutex_);
    if (views_.size() >= maximum_saved_views()) {
      views_.erase(view_order_.front());
      view_order_.pop_front();
    }
    views_[id] = view;
    view_order_.push_back(id);
  }
  if (journal_) {
    try {
      append_operational(
          "saved_task",
          Json{{"task", view.at("task")},
               {"view_id", id},
               {"model_id", view.at("model_id")},
               {"model_revision", view.at("model_revision")},
               {"timestamp_ms", unix_time_ms()}});
    } catch (...) {
      std::lock_guard<std::mutex> view_lock(view_mutex_);
      views_.erase(id);
      const auto location = std::find(view_order_.begin(), view_order_.end(), id);
      if (location != view_order_.end()) {
        view_order_.erase(location);
      }
      throw;
    }
  }
  return id;
}

std::optional<Json> Runtime::trusted_view(const Json& supplied) const {
  if (!supplied.is_object() || !supplied.contains("view_id") ||
      !supplied.at("view_id").is_string()) {
    return std::nullopt;
  }
  const auto id = supplied.at("view_id").get<std::string>();
  std::lock_guard<std::mutex> lock(view_mutex_);
  const auto found = views_.find(id);
  if (found == views_.end()) {
    return std::nullopt;
  }
  if (supplied.size() != 1 && supplied != found->second) {
    return Json();
  }
  return found->second;
}

Json Runtime::reconcile(const Json& supplied_view) {
  const auto canonical = trusted_view(supplied_view);
  if (!canonical.has_value()) {
    throw DomainError{"unknown_view", "view_id is not a known server view"};
  }
  if (canonical->is_null()) {
    throw DomainError{"view_tampered",
                      "supplied view differs from the trusted server view"};
  }
  auto [model, generation] = model_snapshot();
  auto revised = reconcile_view(model, *canonical);
  std::lock_guard<std::mutex> model_lock(model_mutex_);
  if (model_generation_.load(std::memory_order_relaxed) != generation) {
    throw DomainError{"stale_context", "active model changed while reconciling view"};
  }
  revised["view_id"] = canonical->at("view_id");
  revised["session_id"] = session_id_;
  revised["context_generation"] = generation;
  std::lock_guard<std::mutex> view_lock(view_mutex_);
  const auto found = views_.find(revised.at("view_id").get<std::string>());
  if (found == views_.end() || found->second != *canonical) {
    throw DomainError{"lost_update", "view changed while reconciling"};
  }
  found->second = revised;
  return revised;
}

}  /* namespace context_hmi::service */
