#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace context_hmi {
using Json = nlohmann::json;

class InferenceError : public std::runtime_error {
  public:
    InferenceError(std::string code, std::string message);
    const std::string &code() const noexcept {
        return code_;
    }

  private:
    std::string code_;
};

struct InferenceConfig {
    std::string llama_url;
    std::chrono::milliseconds timeout{5000};
    std::size_t max_prompt_bytes{8192};
    std::size_t max_response_bytes{65536};
};

// A provider returns only an interpretation envelope. Binding the task to a
// view remains the engine's responsibility.
class InferenceProvider {
  public:
    virtual ~InferenceProvider() = default;
    virtual Json interpret(const Json &model, const std::string &prompt) = 0;
    virtual std::string name() const = 0;
};

class RuleInferenceProvider final : public InferenceProvider {
  public:
    Json interpret(const Json &model, const std::string &prompt) override;
    std::string name() const override {
        return "rules";
    }
};

class LlamaInferenceProvider final : public InferenceProvider {
  public:
    explicit LlamaInferenceProvider(InferenceConfig config);
    Json interpret(const Json &model, const std::string &prompt) override;
    std::string name() const override {
        return "llama.cpp";
    }

  private:
    InferenceConfig config_;
    std::string host_;
    std::string path_;
    int port_{0};
};

// Selects exactly one provider. An unavailable llama provider is an error and
// does not silently change the interpreter mode.
class Inference {
  public:
    explicit Inference(InferenceConfig config = {});
    Json interpret(const Json &model, const std::string &prompt,
                   const std::string &requested_mode = {});
    std::string mode() const;

  private:
    InferenceConfig config_;
    RuleInferenceProvider rules_;
    std::unique_ptr<LlamaInferenceProvider> llama_;
};

// Validates the compact schema emitted by an inference provider.
Json validate_interpretation(const Json &value);

} // namespace context_hmi
