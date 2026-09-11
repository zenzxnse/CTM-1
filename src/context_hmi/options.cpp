#include "context_hmi/options.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace context_hmi {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t kMaxConfigBytes = 64U * 1024U;
constexpr std::size_t kMaxConfigString = 4096U;

int bounded_integer(const std::string& text, int minimum, int maximum, const char* name) {
  int result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || error != std::errc() || end != text.data() + text.size() ||
      result < minimum || result > maximum) {
    throw std::runtime_error(std::string(name) + " must be between " +
                             std::to_string(minimum) + " and " + std::to_string(maximum));
  }
  return result;
}

bool loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool valid_environment_name(const std::string& name) {
  if (name.empty() || name.size() > 128U ||
      !(name.front() == '_' || (name.front() >= 'A' && name.front() <= 'Z'))) {
    return false;
  }
  return std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
    return character == '_' || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
  });
}

bool valid_workbench_origin(const std::string& origin) {
  constexpr const char* kHttp = "http://";
  if (origin.rfind(kHttp, 0) != 0) {
    return false;
  }
  const auto authority = origin.substr(7);
  if (authority.empty() || authority.find('/') != std::string::npos ||
      authority.find('@') != std::string::npos) {
    return false;
  }
  const auto colon = authority.rfind(':');
  if (colon == std::string::npos || !loopback_host(authority.substr(0, colon))) {
    return false;
  }
  try {
    static_cast<void>(bounded_integer(authority.substr(colon + 1), 1, 65535, "origin port"));
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

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
  if (!object.at(key).is_string() || object.at(key).get_ref<const std::string&>().size() >
                                         kMaxConfigString) {
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
    options.model_rescan_ms = optional_integer(machine, "rescan_ms", options.model_rescan_ms,
                                               100, 60000, "configuration.machine");
  }
  if (config.contains("telemetry")) {
    const auto& source = config.at("telemetry");
    require_allowed(source, {"stale_after_ms", "max_values", "max_batch_bytes"},
                    "configuration.telemetry");
    options.telemetry_stale_after_ms = optional_integer(
        source, "stale_after_ms", static_cast<int>(options.telemetry_stale_after_ms),
        100, 300000,
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
                                            1, 86400, "configuration.cache") * 1000;
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
    require_allowed(diagnostics, {"file", "queue_capacity", "rotation_bytes",
                                  "rotation_files"},
                    "configuration.diagnostics");
    options.diagnostic_log_file =
        optional_string(diagnostics, "file", options.diagnostic_log_file,
                        "configuration.diagnostics");
    options.diagnostic_queue_capacity = optional_integer(
        diagnostics, "queue_capacity", options.diagnostic_queue_capacity, 16, 65536,
        "configuration.diagnostics");
    options.diagnostic_rotation_bytes = static_cast<std::uint64_t>(optional_integer(
        diagnostics, "rotation_bytes",
        static_cast<int>(options.diagnostic_rotation_bytes), 4096,
        std::numeric_limits<int>::max(), "configuration.diagnostics"));
    options.diagnostic_rotation_files = optional_integer(
        diagnostics, "rotation_files", options.diagnostic_rotation_files, 1, 32,
        "configuration.diagnostics");
  }
  options.config_file = path;
  return options;
}

void validate(RuntimeOptions& options) {
  if (options.port < 1 || options.port > 65535) {
    throw std::runtime_error("port out of range");
  }
  if (options.provider_timeout_ms < 100 || options.provider_timeout_ms > 30000) {
    throw std::runtime_error("provider timeout must be between 100 and 30000 ms");
  }
  if (options.model.empty() || options.models_dir.empty()) {
    throw std::runtime_error("model and models directory must not be empty");
  }
  if (options.operator_actor.empty() || options.operator_actor.size() > 128U) {
    throw std::runtime_error("audit actor must be a nonempty string of at most 128 bytes");
  }
  if (options.store_max_bytes < 1024U * 1024U ||
      options.store_max_bytes > 4ULL * 1024ULL * 1024ULL * 1024ULL) {
    throw std::runtime_error("operational store maximum must be from 1 MiB through 4 GiB");
  }
  if (options.diagnostic_queue_capacity < 16 ||
      options.diagnostic_queue_capacity > 65536 ||
      options.diagnostic_rotation_bytes < 4096 ||
      options.diagnostic_rotation_files < 1 ||
      options.diagnostic_rotation_files > 32) {
    throw std::runtime_error("diagnostic logger limits are out of range");
  }
  if (!options.workbench_origin.empty() && !valid_workbench_origin(options.workbench_origin)) {
    throw std::runtime_error("workbench_origin must be an http:// loopback origin with a port");
  }
  if (!options.allow_remote && !loopback_host(options.host)) {
    throw std::runtime_error("non-loopback bind requires explicit allow_remote");
  }
  if (!options.provider_api_key_env.empty()) {
    if (!valid_environment_name(options.provider_api_key_env)) {
      throw std::runtime_error("provider key environment name is invalid");
    }
    const char* key = std::getenv(options.provider_api_key_env.c_str());
    if (key == nullptr || *key == '\0') {
      throw std::runtime_error("configured provider key environment variable is empty");
    }
    options.provider_api_key = key;
  }
}

}  /* namespace */

OptionParseResult parse_options(int argc, char** argv) {
  std::string config_path;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--config") {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value for --config");
      }
      config_path = argv[++index];
    }
  }

  OptionParseResult result;
  if (!config_path.empty()) {
    result.options = load_config(config_path);
  }

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&](const char* name) -> std::string {
      if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++index];
    };
    if (argument == "--config") {
      static_cast<void>(next("--config"));
    } else if (argument == "--host") {
      result.options.host = next("--host");
    } else if (argument == "--port") {
      result.options.port = bounded_integer(next("--port"), 1, 65535, "port");
    } else if (argument == "--model") {
      result.options.model = next("--model");
    } else if (argument == "--models") {
      result.options.models_dir = next("--models");
    } else if (argument == "--no-model-watch") {
      result.options.watch_model = false;
    } else if (argument == "--model-rescan-ms") {
      result.options.model_rescan_ms = bounded_integer(next("--model-rescan-ms"), 100, 60000,
                                                       "model rescan interval");
    } else if (argument == "--telemetry-stale-after-ms") {
      result.options.telemetry_stale_after_ms = bounded_integer(
          next("--telemetry-stale-after-ms"), 100, 300000,
          "telemetry stale interval");
    } else if (argument == "--telemetry-max-values") {
      result.options.telemetry_max_values = bounded_integer(
          next("--telemetry-max-values"), 1, 4096, "telemetry value limit");
    } else if (argument == "--telemetry-max-batch-bytes") {
      result.options.telemetry_max_batch_bytes = bounded_integer(
          next("--telemetry-max-batch-bytes"), 1024, 1048576,
          "telemetry batch byte limit");
    } else if (argument == "--web-dir") {
      result.options.web_dir = next("--web-dir");
    } else if (argument == "--llama-url" || argument == "--provider-url") {
      result.options.provider_url = next(argument.c_str());
    } else if (argument == "--provider-model") {
      result.options.provider_model = next("--provider-model");
    } else if (argument == "--provider-context-bytes") {
      result.options.provider_context_bytes = bounded_integer(
          next("--provider-context-bytes"), 1024, 65536, "provider context bytes");
    } else if (argument == "--provider-max-output-tokens") {
      result.options.provider_max_output_tokens = bounded_integer(
          next("--provider-max-output-tokens"), 64, 2048,
          "provider maximum output tokens");
    } else if (argument == "--provider-key-env") {
      result.options.provider_api_key_env = next("--provider-key-env");
    } else if (argument == "--llama-timeout-ms" || argument == "--provider-timeout-ms") {
      result.options.provider_timeout_ms =
          bounded_integer(next(argument.c_str()), 100, 30000, "provider timeout");
    } else if (argument == "--allow-remote") {
      result.options.allow_remote = true;
    } else if (argument == "--allow-remote-inference") {
      result.options.allow_remote_inference = true;
    } else if (argument == "--retrieval-cache-capacity") {
      result.options.retrieval_cache_capacity = bounded_integer(
          next("--retrieval-cache-capacity"), 1, 4096, "retrieval cache capacity");
    } else if (argument == "--interpretation-cache-capacity") {
      result.options.interpretation_cache_capacity = bounded_integer(
          next("--interpretation-cache-capacity"), 1, 4096,
          "interpretation cache capacity");
    } else if (argument == "--cache-ttl-ms") {
      result.options.cache_ttl_ms = bounded_integer(next("--cache-ttl-ms"), 1000, 86400000,
                                                    "cache TTL");
    } else if (argument == "--workbench-origin") {
      result.options.workbench_origin = next("--workbench-origin");
    } else if (argument == "--store-file") {
      result.options.store_file = next("--store-file");
    } else if (argument == "--store-no-sync") {
      result.options.store_sync = false;
    } else if (argument == "--store-max-bytes") {
      result.options.store_max_bytes = static_cast<std::uint64_t>(bounded_integer(
          next("--store-max-bytes"), 1024 * 1024, std::numeric_limits<int>::max(),
          "operational store maximum"));
    } else if (argument == "--log-file") {
      result.options.diagnostic_log_file = next("--log-file");
    } else if (argument == "--log-queue") {
      result.options.diagnostic_queue_capacity =
          bounded_integer(next("--log-queue"), 16, 65536, "diagnostic queue");
    } else if (argument == "--log-rotation-bytes") {
      result.options.diagnostic_rotation_bytes = static_cast<std::uint64_t>(
          bounded_integer(next("--log-rotation-bytes"), 4096,
                          std::numeric_limits<int>::max(),
                          "diagnostic rotation bytes"));
    } else if (argument == "--log-rotation-files") {
      result.options.diagnostic_rotation_files =
          bounded_integer(next("--log-rotation-files"), 1, 32,
                          "diagnostic rotation files");
    } else if (argument == "--operator-actor") {
      result.options.operator_actor = next("--operator-actor");
    } else if (argument == "--help") {
      result.show_help = true;
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  validate(result.options);
  return result;
}

std::string options_help() {
  return "context-hmi [--config FILE] [--host HOST] [--port PORT] [--model FILE] "
         "[--models DIR] [--no-model-watch] [--model-rescan-ms MS] "
         "[--telemetry-stale-after-ms MS] [--telemetry-max-values COUNT] "
         "[--telemetry-max-batch-bytes BYTES] [--web-dir DIR] "
         "[--llama-url URL|--provider-url URL] "
         "[--provider-model NAME] [--provider-key-env NAME] "
         "[--llama-timeout-ms MS|--provider-timeout-ms MS] "
         "[--provider-context-bytes BYTES] [--provider-max-output-tokens COUNT] "
         "[--retrieval-cache-capacity COUNT] [--interpretation-cache-capacity COUNT] "
         "[--cache-ttl-ms MS] "
         "[--workbench-origin http://127.0.0.1:5173] "
         "[--store-file FILE] [--store-max-bytes BYTES] [--store-no-sync] "
         "[--log-file FILE] [--log-queue COUNT] [--log-rotation-bytes BYTES] "
         "[--log-rotation-files COUNT] "
         "[--operator-actor ACTOR] [--allow-remote-inference] [--allow-remote]\n";
}

}  /* namespace context_hmi */
