#include "context_hmi/http_common.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace context_hmi::http {
namespace {

enum class JsonShape { kOk, kTooDeep, kIncomplete };

JsonShape inspect_json_shape(std::string_view input) {
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

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   if (character >= 'A' && character <= 'Z') {
                     return static_cast<char>(character - 'A' + 'a');
                   }
                   return static_cast<char>(character);
                 });
  return value;
}

bool origin_allowed(const drogon::HttpRequestPtr& request,
                    const RuntimeOptions& options) {
  const auto origin = request->getHeader("origin");
  if (origin.empty()) {
    return true;
  }
  const auto host = options.host.find(':') == std::string::npos
                        ? options.host
                        : "[" + options.host + "]";
  const auto port = std::to_string(options.port);
  return origin == "http://" + host + ":" + port ||
         origin == "http://127.0.0.1:" + port ||
         origin == "http://localhost:" + port ||
         (!options.workbench_origin.empty() &&
          origin == options.workbench_origin);
}

std::string request_id(const drogon::HttpRequestPtr& request) {
  const auto supplied = request->getHeader("x-request-id");
  if (!supplied.empty() && supplied.size() <= 128 &&
      std::all_of(supplied.begin(), supplied.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.';
      })) {
    return supplied;
  }
  static std::atomic<std::uint64_t> sequence{0};
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  return "request-" + std::to_string(now) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

}  /* namespace */

Json error_json(const std::string& code, const std::string& message) {
  return Json{{"error", Json{{"code", code}, {"message", message}}}};
}

drogon::HttpResponsePtr json_response(const Json& value, int status) {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  response->setContentTypeString("application/json");
  response->setBody(value.dump());
  response->addHeader("Cache-Control", "no-store");
  return response;
}

std::optional<Json> parse_body(const drogon::HttpRequestPtr& request,
                               const ResponseCallback& callback,
                               std::size_t maximum_bytes) {
  const auto body = request->body();
  if (maximum_bytes == 0 || maximum_bytes > 1024U * 1024U ||
      body.size() > maximum_bytes) {
    callback(json_response(
        error_json("body_too_large", "request body exceeds maximum size"), 413));
    return std::nullopt;
  }
  const auto shape = inspect_json_shape(body);
  if (shape == JsonShape::kTooDeep) {
    callback(json_response(error_json(
                               "json_too_deep",
                               "request JSON nesting exceeds maximum depth"),
                           400));
    return std::nullopt;
  }
  if (shape == JsonShape::kIncomplete) {
    callback(json_response(
        error_json("invalid_json", "request JSON is incomplete"), 400));
    return std::nullopt;
  }
  try {
    auto value = Json::parse(body);
    if (!value.is_object()) {
      throw std::runtime_error("request JSON must be an object");
    }
    return value;
  } catch (const std::exception& error) {
    callback(json_response(error_json("invalid_json", error.what()), 400));
    return std::nullopt;
  }
}

bool mutation_guard(const drogon::HttpRequestPtr& request,
                    const ResponseCallback& callback,
                    const RuntimeOptions& options) {
  std::string content_type = lower_ascii(request->getHeader("content-type"));
  const auto parameter = content_type.find(';');
  if (parameter != std::string::npos) {
    content_type.resize(parameter);
  }
  while (!content_type.empty() &&
         std::isspace(static_cast<unsigned char>(content_type.back())) != 0) {
    content_type.pop_back();
  }
  if (content_type != "application/json") {
    callback(json_response(error_json(
                               "content_type",
                               "mutation requests require application/json"),
                           415));
    return false;
  }
  if (!origin_allowed(request, options)) {
    callback(json_response(error_json(
                               "cross_origin",
                               "cross-origin mutation is not permitted"),
                           403));
    return false;
  }
  return true;
}

std::optional<std::size_t> bounded_limit(const drogon::HttpRequestPtr& request,
                                         const ResponseCallback& callback,
                                         std::size_t default_value) {
  const auto value = request->getParameter("limit");
  if (value.empty()) {
    return default_value;
  }
  std::size_t parsed = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (conversion.ec != std::errc() || conversion.ptr != value.data() + value.size() ||
      parsed < 1 || parsed > 100) {
    callback(json_response(
        error_json("invalid_limit", "audit limit must be from 1 through 100"), 400));
    return std::nullopt;
  }
  return parsed;
}

int domain_status(const DomainError& error) {
  if (error.code == "scenario_not_found") {
    return 404;
  }
  if (error.code == "command_denied") {
    return 403;
  }
  if (error.code == "stale_context" || error.code == "lost_update" ||
      error.code == "idempotency_conflict" ||
      error.code == "confirmation_required" ||
      error.code == "stale_model_revision") {
    return 409;
  }
  if (error.code == "audit_unavailable") {
    return 503;
  }
  if (error.code == "unknown_view" || error.code == "view_tampered") {
    return 422;
  }
  if (error.code == "invalid_command" || error.code == "invalid_scenario") {
    return 400;
  }
  if (error.code == "invalid_request") {
    return 400;
  }
  return 422;
}

int inference_status(const InferenceError& error) {
  if (error.code() == "stale_context") {
    return 409;
  }
  if (error.code() == "inference_busy") {
    return 429;
  }
  if (error.code() == "invalid_interpreter" ||
      error.code() == "invalid_prompt" || error.code() == "prompt_too_large") {
    return 400;
  }
  if (error.code() == "provider_unconfigured") {
    return 409;
  }
  return 502;
}

void configure_framework(drogon::HttpAppFramework& app,
                         const RuntimeOptions& options) {
  const auto body_limit = std::max(
      kMaximumBodyBytes,
      static_cast<std::size_t>(options.telemetry_max_batch_bytes));
  app.setClientMaxBodySize(body_limit)
      .setClientMaxMemoryBodySize(body_limit)
      .setThreadNum(8)
      .enableSession(1200, drogon::Cookie::SameSite::kStrict,
                     "CONTEXT_HMI_SESSION")
      .addListener(options.host, static_cast<std::uint16_t>(options.port));
  app.registerPreSendingAdvice(
      [](const drogon::HttpRequestPtr& request,
         const drogon::HttpResponsePtr& response) {
        response->addHeader("X-Content-Type-Options", "nosniff");
        response->addHeader("X-Frame-Options", "DENY");
        response->addHeader("Referrer-Policy", "no-referrer");
        response->addHeader("Permissions-Policy",
                            "camera=(), geolocation=(), microphone=(self)");
        const auto& path = request->path();
        if (path == "/" || path.ends_with(".html")) {
          response->addHeader(
              "Content-Security-Policy",
              "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
        } else {
          response->addHeader(
              "Content-Security-Policy",
              "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
              "script-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self'; "
              "base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
        }
        response->addHeader("X-Request-ID", request_id(request));
      });
  app.setCustomErrorHandler([](drogon::HttpStatusCode status) {
    const auto code = static_cast<int>(status);
    if (code == 413) {
      return json_response(
          error_json("body_too_large", "request body exceeds maximum size"), 413);
    }
    if (code == 405) {
      return json_response(error_json(
                               "method_not_allowed",
                               "method is not allowed for this route"),
                           405);
    }
    return json_response(error_json("http_error", "request could not be processed"),
                         code);
  });
  app.setCustom404Page(json_response(error_json("not_found", "route not found"), 404),
                       true);
  if (!options.web_dir.empty()) {
    std::error_code error;
    if (!std::filesystem::is_directory(options.web_dir, error)) {
      throw std::runtime_error("web directory does not exist");
    }
    app.setDocumentRoot(options.web_dir).setHomePage("index.html");
  }
}

}  /* namespace context_hmi::http */
