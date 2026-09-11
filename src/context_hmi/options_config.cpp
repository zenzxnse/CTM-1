#include "context_hmi/options_internal.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace context_hmi::options_internal {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t kMaxConfigBytes = 64U * 1024U;
constexpr std::size_t kMaxConfigString = 4096U;

void require_allowed(const Json& value, const std::set<std::string>& names,
                     const std::string& location) {
  if (!value.is_object()) {
    throw std::runtime_error(location + " must be an object");
  }
  for (const auto& item : value.items()) {
    if (!names.contains(item.key())) {
      throw std::runtime_error(location + " contains unsupported field '" + item.key() + "'");
    }
  }
}

std::string optional_string(const Json& object, const char* key, const std::string& fallback,
                            const std::string& location) {
  if (!object.contains(key)) {
    return fallback;
  }
  if (!object.at(key).is_string() ||
      object.at(key).get_ref<const std::string&>().size() > kMaxConfigString) {
    throw std::runtime_error(location + "." + key + " must be a bounded string");
  }
  return object.at(key).get<std::string>();
}

bool optional_boolean(const Json& object, const char* key, bool fallback,
                      const std::string& location) {
  if (!object.contains(key)) {
    return fallback;
  }
  if (!object.at(key).is_boolean()) {
    throw std::runtime_error(location + "." + key + " must be boolean");
  }
  return object.at(key).get<bool>();
}

int optional_integer(const Json& object, const char* key, int fallback, int minimum, int maximum,
                     const std::string& location) {
  if (!object.contains(key)) {
    return fallback;
  }
  if (!object.at(key).is_number_integer()) {
    throw std::runtime_error(location + "." + key + " must be an integer");
  }
  const auto number = object.at(key).get<std::int64_t>();
  if (number < minimum || number > maximum) {
    throw std::runtime_error(location + "." + key + " is out of range");
  }
  return static_cast<int>(number);
}

std::string read_config_file(const fs::path& path) {
  std::error_code error;
  if (!fs::is_regular_file(path, error)) {
    throw std::runtime_error("configuration file is missing or is not a regular file");
  }
  const auto size = fs::file_size(path, error);
  if (error || size > kMaxConfigBytes) {
    throw std::runtime_error("configuration file exceeds 64 KiB");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open configuration file");
  }
  std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (body.size() > kMaxConfigBytes) {
    throw std::runtime_error("configuration file exceeds 64 KiB");
  }
  return body;
}

}  /* namespace */

RuntimeOptions load_config(const std::string& path) {
  RuntimeOptions options;
  const std::string body = read_config_file(path);
  Json config;
  try {
    config = Json::parse(body, [](int depth, Json::parse_event_t, Json&) {
      if (depth > 32) {
        throw std::runtime_error("configuration JSON exceeds nesting limit");
      }
      return true;
    });
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("invalid configuration: ") + error.what());
  }
  require_allowed(config,
                  {"schema_version", "server", "machine", "telemetry", "inference", "cache",
                   "audit", "storage", "diagnostics"},
                  "configuration");
  if (!config.contains("schema_version") || !config.at("schema_version").is_number_integer() ||
      config.at("schema_version") != 1) {
    throw std::runtime_error("configuration.schema_version must be 1");
  }
  if (config.contains("server")) {
    const auto& server = config.at("server");
    require_allowed(server, {"host", "port", "allow_remote", "workbench_origin", "web_dir"},
                    "configuration.server");
    options.host = optional_string(server, "host", options.host, "configuration.server");
    options.port = optional_integer(server, "port", options.port, 1, 65535,
                                    "configuration.server");
    options.allow_remote = optional_boolean(server, "allow_remote", options.allow_remote,
                                            "configuration.server");
    options.workbench_origin = optional_string(server, "workbench_origin",
                                               options.workbench_origin, "configuration.server");
    options.web_dir = optional_string(server, "web_dir", options.web_dir, "configuration.server");
  }
  if (config.contains("machine")) {
    const auto& machine = config.at("machine");
    require_allowed(machine, {"model", "models_dir", "watch", "rescan_ms"},
                    "configuration.machine");
    options.model = optional_string(machine, "model", options.model, "configuration.machine");
    options.models_dir =
        optional_string(machine, "models_dir", options.models_dir, "configuration.machine");
    options.watch_model =
        optional_boolean(machine, "watch", options.watch_model, "configuration.machine");
    options.model_rescan_ms = optional_integer(machine, "rescan_ms", options.model_rescan_ms, 100,
                                               60000, "configuration.machine");
  }
  if (config.contains("telemetry")) {
    const auto& source = config.at("telemetry");
    require_allowed(source, {"stale_after_ms", "max_values", "max_batch_bytes"},
                    "configuration.telemetry");
    options.telemetry_stale_after_ms = optional_integer(
        source, "stale_after_ms", static_cast<int>(options.telemetry_stale_after_ms), 100, 300000,
        "configuration.telemetry");
    options.telemetry_max_values = optional_integer(
        source, "max_values", options.telemetry_max_values, 1, 4096,
        "configuration.telemetry");
    options.telemetry_max_batch_bytes = optional_integer(
        source, "max_batch_bytes", options.telemetry_max_batch_bytes, 1024, 1048576,
        "configuration.telemetry");
  }
  if (config.contains("inference")) {
    const auto& inference = config.at("inference");
    require_allowed(inference, {"endpoint", "model", "timeout_ms", "allow_remote", "key_env",
                                "context_bytes", "max_output_tokens"},
                    "configuration.inference");
    options.provider_url =
        optional_string(inference, "endpoint", options.provider_url, "configuration.inference");
    options.provider_model =
        optional_string(inference, "model", options.provider_model, "configuration.inference");
    options.provider_timeout_ms = optional_integer(inference, "timeout_ms",
                                                   options.provider_timeout_ms, 100, 30000,
                                                   "configuration.inference");
    options.provider_context_bytes = optional_integer(
        inference, "context_bytes", options.provider_context_bytes, 1024, 65536,
        "configuration.inference");
    options.provider_max_output_tokens = optional_integer(
        inference, "max_output_tokens", options.provider_max_output_tokens, 64, 2048,
        "configuration.inference");
    options.allow_remote_inference = optional_boolean(inference, "allow_remote",
                                                      options.allow_remote_inference,
                                                      "configuration.inference");
    options.provider_api_key_env = optional_string(inference, "key_env",
                                                   options.provider_api_key_env,
                                                   "configuration.inference");
  }
  if (config.contains("cache")) {
    const auto& cache = config.at("cache");
    require_allowed(cache, {"retrieval_capacity", "interpretation_capacity", "ttl_seconds"},
                    "configuration.cache");
    options.retrieval_cache_capacity = optional_integer(
        cache, "retrieval_capacity", options.retrieval_cache_capacity, 1, 4096,
        "configuration.cache");
    options.interpretation_cache_capacity = optional_integer(
        cache, "interpretation_capacity", options.interpretation_cache_capacity, 1, 4096,
        "configuration.cache");
    options.cache_ttl_ms = optional_integer(cache, "ttl_seconds", options.cache_ttl_ms / 1000,
                                            1, 86400, "configuration.cache") *
                           1000;
  }
  if (config.contains("audit")) {
    const auto& audit = config.at("audit");
    require_allowed(audit, {"actor"}, "configuration.audit");
    options.operator_actor =
        optional_string(audit, "actor", options.operator_actor, "configuration.audit");
  }
  if (config.contains("storage")) {
    const auto& storage = config.at("storage");
    require_allowed(storage, {"file", "synchronize", "maximum_bytes"},
                    "configuration.storage");
    options.store_file =
        optional_string(storage, "file", options.store_file, "configuration.storage");
    options.store_sync = optional_boolean(storage, "synchronize", options.store_sync,
                                          "configuration.storage");
    if (storage.contains("maximum_bytes")) {
      if (!storage.at("maximum_bytes").is_number_unsigned()) {
        throw std::runtime_error("configuration.storage.maximum_bytes must be unsigned");
      }
      options.store_max_bytes = storage.at("maximum_bytes").get<std::uint64_t>();
    }
  }
  if (config.contains("diagnostics")) {
    const auto& diagnostics = config.at("diagnostics");
    require_allowed(diagnostics, {"file", "queue_capacity", "rotation_bytes", "rotation_files"},
                    "configuration.diagnostics");
    options.diagnostic_log_file =
        optional_string(diagnostics, "file", options.diagnostic_log_file,
                        "configuration.diagnostics");
    options.diagnostic_queue_capacity = optional_integer(
        diagnostics, "queue_capacity", options.diagnostic_queue_capacity, 16, 65536,
        "configuration.diagnostics");
    options.diagnostic_rotation_bytes = static_cast<std::uint64_t>(optional_integer(
        diagnostics, "rotation_bytes", static_cast<int>(options.diagnostic_rotation_bytes), 4096,
        std::numeric_limits<int>::max(), "configuration.diagnostics"));
    options.diagnostic_rotation_files = optional_integer(
        diagnostics, "rotation_files", options.diagnostic_rotation_files, 1, 32,
        "configuration.diagnostics");
  }
  options.config_file = path;
  return options;
}

}  /* namespace context_hmi::options_internal */
