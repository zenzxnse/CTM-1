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
  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

struct InferenceConfig {
  std::string llama_url;
  std::chrono::milliseconds timeout{5000};
  std::size_t max_prompt_bytes{8192};
  std::size_t max_response_bytes{65536};
  std::size_t max_context_bytes{12288};
  int max_output_tokens{256};
  std::string model{"local-model"};
  std::string api_key;
  bool allow_remote{false};
};

/** A provider proposes an interpretation envelope. The engine owns every binding. */
class InferenceProvider {
 public:
  virtual ~InferenceProvider() = default;
  virtual Json interpret(const Json& model, const std::string& prompt,
                         const Json& retrieval) = 0;
  virtual Json readiness() = 0;
  virtual std::string name() const = 0;
};

class RuleInferenceProvider final : public InferenceProvider {
 public:
  Json interpret(const Json& model, const std::string& prompt,
                 const Json& retrieval) override;
  Json readiness() override;
  std::string name() const override { return "rules"; }
};

class LlamaInferenceProvider final : public InferenceProvider {
 public:
  explicit LlamaInferenceProvider(InferenceConfig config);
  Json interpret(const Json& model, const std::string& prompt,
                 const Json& retrieval) override;
  Json readiness() override;
  std::string name() const override;

 private:
  InferenceConfig config_;
  std::string scheme_host_port_;
  std::string host_;
  std::string path_;
  std::string models_path_;
  int port_{0};
  bool tls_{false};
  bool loopback_{true};
  bool groq_structured_profile_{false};
};

/** Selects one explicit provider and never converts provider failure into rules mode. */
class Inference {
 public:
  explicit Inference(InferenceConfig config = {});
  Json interpret(const Json& model, const std::string& prompt,
                 const Json& retrieval,
                 const std::string& requested_mode = {});
  Json readiness();
  std::string mode() const;

 private:
  InferenceConfig config_;
  RuleInferenceProvider rules_;
  std::unique_ptr<LlamaInferenceProvider> llama_;
};

/** Validates the compact schema emitted by any inference provider. */
Json validate_interpretation(const Json& value);

}  /* namespace context_hmi */
