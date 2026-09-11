#include "inference.hpp"

#include <memory>
#include <string>
#include <utility>

#include "context_hmi/context_retrieval.hpp"
#include "engine.hpp"

namespace context_hmi {

Inference::Inference(InferenceConfig config) : config_(std::move(config)) {
  if (!config_.llama_url.empty()) {
    llama_ = std::make_unique<LlamaInferenceProvider>(config_);
  }
}

Json Inference::interpret(const Json& model, const std::string& prompt, const Json& retrieval,
                          const std::string& requested_mode) {
  const std::string mode =
      requested_mode.empty() ? (llama_ ? "llama" : "rules") : requested_mode;
  Json result;
  if (mode == "rules") {
    result = rules_.interpret(model, prompt, retrieval);
    result["interpreter"] = "rules";
    result["provider_usage"] = Json{{"reported", false}, {"provider_call", false}};
  } else if (mode == "llama" || mode == "llama.cpp") {
    if (!llama_) {
      throw InferenceError("provider_unconfigured", "AI interpreter was not configured");
    }
    result = llama_->interpret(model, prompt, retrieval);
    result["interpreter"] = llama_->name();
  } else {
    throw InferenceError("invalid_interpreter", "interpreter must be rules or llama");
  }
  try {
    context::validate_interpretation_scope(model, retrieval, result);
  } catch (const DomainError& error) {
    throw InferenceError(error.code, error.details);
  }
  result["retrieval"] = retrieval;
  return result;
}

Json Inference::readiness() {
  if (llama_) {
    return llama_->readiness();
  }
  return rules_.readiness();
}

std::string Inference::mode() const {
  return llama_ ? llama_->name() : "rules";
}

}  /* namespace context_hmi */
