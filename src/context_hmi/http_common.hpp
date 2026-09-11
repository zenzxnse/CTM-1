#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "context_hmi/engine.hpp"
#include "context_hmi/inference.hpp"
#include "context_hmi/options.hpp"
#include "context_hmi/worker_pool.hpp"

namespace context_hmi::http {

using Json = nlohmann::json;
using ResponseCallback =
    std::function<void(const drogon::HttpResponsePtr&)>;

inline constexpr std::size_t kMaximumBodyBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumPromptBytes = 8192;

struct JsonResult {
  Json body;
  int status{200};
};

Json error_json(const std::string& code, const std::string& message);
drogon::HttpResponsePtr json_response(const Json& value, int status = 200);
std::optional<Json> parse_body(const drogon::HttpRequestPtr& request,
                               const ResponseCallback& callback,
                               std::size_t maximum_bytes = kMaximumBodyBytes);
bool mutation_guard(const drogon::HttpRequestPtr& request,
                    const ResponseCallback& callback,
                    const RuntimeOptions& options);
std::optional<std::size_t> bounded_limit(const drogon::HttpRequestPtr& request,
                                         const ResponseCallback& callback,
                                         std::size_t default_value);
int domain_status(const DomainError& error);
int inference_status(const InferenceError& error);
void configure_framework(drogon::HttpAppFramework& app,
                         const RuntimeOptions& options);

template <typename Function>
void dispatch_json(execution::WorkerPool& pool, ResponseCallback callback,
                   Function function) {
  try {
    pool.dispatch([callback = std::move(callback),
                   function = std::move(function)]() mutable {
      try {
        auto result = function();
        callback(json_response(result.body, result.status));
      } catch (const InferenceError& error) {
        callback(json_response(error_json(error.code(), error.what()),
                               inference_status(error)));
      } catch (const DomainError& error) {
        callback(json_response(error_json(error.code, error.details),
                               domain_status(error)));
      } catch (const std::exception&) {
        callback(json_response(
            error_json("engine_error", "request could not be processed"), 500));
      }
    });
  } catch (const execution::QueueFull& error) {
    callback(json_response(error_json("execution_busy", error.what()), 429));
  }
}

}  /* namespace context_hmi::http */
