#include "inference.hpp"

#include <utility>

namespace context_hmi {
namespace {

Json rule_interpretation(const Json& model, const std::string& prompt, const Json& retrieval) {
  Json result{{"interpreter", "rules"},
              {"prompt", prompt},
              {"status", "clarification"},
              {"message", "Specify whether to show measurements or alarms"},
              {"candidates", Json::array()},
              {"supported_tasks", Json::array({"overview", "alarms"})}};
  for (const auto& candidate : retrieval.at("candidates")) {
    result["candidates"].push_back(
        Json{{"asset_id", candidate.at("asset_id")}, {"name", candidate.at("name")}});
  }
  if (retrieval.value("requires_clarification", false)) {
    result["message"] = retrieval.at("eligible_asset_ids").size() > 1
                             ? "The equipment reference is ambiguous; choose one asset"
                             : "The equipment reference does not match declared context";
    return result;
  }
  const auto& task_hints = retrieval.at("task_hints");
  if (task_hints.size() != 1) {
    return result;
  }
  Json task{{"kind", task_hints.at(0)},
            {"model_id", model.at("model_id")},
            {"model_revision", model.at("revision")},
            {"original_request", prompt}};
  const auto& eligible = retrieval.at("eligible_asset_ids");
  if (eligible.size() == 1) {
    task["anchor_asset_id"] = eligible.at(0);
  }
  const auto& roles = retrieval.at("measurement_role_hints");
  if (!roles.empty()) {
    task["measurement_roles"] = roles;
  }
  result["status"] = "ready";
  result.erase("message");
  result["task"] = std::move(task);
  return result;
}

}  /* namespace */

Json RuleInferenceProvider::interpret(const Json& model, const std::string& prompt,
                                      const Json& retrieval) {
  if (prompt.size() > 8192) {
    throw InferenceError("prompt_too_large", "prompt exceeds maximum size");
  }
  try {
    Json result = rule_interpretation(model, prompt, retrieval);
    return validate_interpretation(result);
  } catch (const InferenceError&) {
    throw;
  } catch (const std::exception& error) {
    throw InferenceError("interpreter_error", error.what());
  }
}

Json RuleInferenceProvider::readiness() {
  return Json{{"provider", "rules"}, {"configured", true}, {"ready", true}};
}

}  /* namespace context_hmi */
