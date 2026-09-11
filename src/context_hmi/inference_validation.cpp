#include "inference.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace context_hmi {
namespace {

bool is_allowed(const Json& object, std::initializer_list<const char*> names) {
  if (!object.is_object()) {
    return false;
  }
  for (const auto& item : object.items()) {
    if (std::find_if(names.begin(), names.end(), [&](const char* name) {
          return item.key() == name;
        }) == names.end()) {
      return false;
    }
  }
  return true;
}

bool is_task_kind(const std::string& kind) {
  return kind == "overview" || kind == "alarms";
}

}  /* namespace */

InferenceError::InferenceError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

Json validate_interpretation(const Json& value) {
  if (!value.is_object()) {
    throw InferenceError("provider_schema", "interpretation must be an object");
  }
  if (!is_allowed(value, {"status", "message", "candidates", "task", "interpreter", "prompt",
                          "supported_tasks"})) {
    throw InferenceError("provider_schema", "interpretation contains unexpected properties");
  }
  if (!value.contains("status") || !value.at("status").is_string()) {
    throw InferenceError("provider_schema", "interpretation status must be a string");
  }
  const auto status = value.at("status").get<std::string>();
  if (status == "ready") {
    if (!value.contains("task") || !value.at("task").is_object()) {
      throw InferenceError("provider_schema", "ready interpretation requires task");
    }
    const auto& task = value.at("task");
    if (!is_allowed(task, {"kind", "anchor_asset_id", "measurement_roles", "model_revision",
                           "original_request", "model_id"})) {
      throw InferenceError("provider_schema", "task contains unexpected properties");
    }
    if (!task.contains("kind") || !task.at("kind").is_string() ||
        !is_task_kind(task.at("kind").get<std::string>())) {
      throw InferenceError("provider_schema", "task kind is unsupported");
    }
    if (task.contains("anchor_asset_id") &&
        (!task.at("anchor_asset_id").is_string() ||
         task.at("anchor_asset_id").get<std::string>().empty())) {
      throw InferenceError("provider_schema",
                           "task anchor_asset_id must be a nonempty string when present");
    }
    if (task.contains("model_id") &&
        (!task.at("model_id").is_string() || task.at("model_id").get<std::string>().empty())) {
      throw InferenceError("provider_schema", "task model_id must be a nonempty string when present");
    }
    if (task.contains("measurement_roles")) {
      if (!task.at("measurement_roles").is_array() || task.at("measurement_roles").empty() ||
          task.at("measurement_roles").size() > 16) {
        throw InferenceError("provider_schema",
                             "task measurement_roles must be a bounded nonempty array");
      }
      for (const auto& role : task.at("measurement_roles")) {
        if (!role.is_string() || role.get<std::string>().empty() ||
            role.get<std::string>().size() > 128) {
          throw InferenceError("provider_schema", "task measurement role is invalid");
        }
      }
    }
    if (!task.contains("model_revision") || !task.at("model_revision").is_number_integer() ||
        task.at("model_revision").get<std::int64_t>() < 0 ||
        task.at("model_revision") > 4294967295ULL) {
      throw InferenceError("provider_schema",
                           "task model_revision must be a nonnegative 32-bit integer");
    }
    if (!task.contains("original_request") || !task.at("original_request").is_string() ||
        task.at("original_request").get<std::string>().size() > 8192) {
      throw InferenceError("provider_schema", "task original_request must be a string");
    }
    if (value.contains("message")) {
      throw InferenceError("provider_schema",
                           "ready interpretation cannot contain clarification fields");
    }
  } else if (status == "clarification") {
    if (!value.contains("message") || !value.at("message").is_string() ||
        value.at("message").get<std::string>().empty() ||
        value.at("message").get<std::string>().size() > 2048) {
      throw InferenceError("provider_schema", "clarification requires message");
    }
    if (value.contains("task")) {
      throw InferenceError("provider_schema", "clarification cannot contain task");
    }
    if (value.contains("candidates")) {
      if (!value.at("candidates").is_array() || value.at("candidates").size() > 32) {
        throw InferenceError("provider_schema",
                             "clarification candidates must be a bounded array");
      }
      for (const auto& candidate : value.at("candidates")) {
        if (candidate.is_string()) {
          if (candidate.get<std::string>().size() > 256) {
            throw InferenceError("provider_schema", "clarification candidate is too long");
          }
        } else if (candidate.is_object() && candidate.size() <= 2 &&
                   candidate.contains("asset_id") && candidate.at("asset_id").is_string() &&
                   candidate.contains("name") && candidate.at("name").is_string()) {
          /** Rules mode includes the display name beside the explicit ID. */
        } else {
          throw InferenceError("provider_schema", "clarification candidate has invalid shape");
        }
      }
    }
  } else {
    throw InferenceError("provider_schema",
                         "interpretation status must be ready or clarification");
  }
  if (value.contains("interpreter") && !value.at("interpreter").is_string()) {
    throw InferenceError("provider_schema", "interpreter must be a string");
  }
  if (value.contains("prompt") &&
      (!value.at("prompt").is_string() || value.at("prompt").get<std::string>().size() > 8192)) {
    throw InferenceError("provider_schema", "prompt must be a bounded string");
  }
  if (value.contains("supported_tasks")) {
    if (!value.at("supported_tasks").is_array() || value.at("supported_tasks").size() > 16) {
      throw InferenceError("provider_schema", "supported_tasks must be a bounded array");
    }
    for (const auto& item : value.at("supported_tasks")) {
      if (!item.is_string()) {
        throw InferenceError("provider_schema", "supported_tasks entries must be strings");
      }
    }
  }
  return value;
}

}  /* namespace context_hmi */
