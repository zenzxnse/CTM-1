#include "context_hmi/options_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>

namespace context_hmi::options_internal {

int bounded_integer(const std::string& text, int minimum, int maximum, const char* name) {
  int result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || error != std::errc() || end != text.data() + text.size() ||
      result < minimum || result > maximum) {
    throw std::runtime_error(std::string(name) + " must be between " +
                             std::to_string(minimum) + " and " +
                             std::to_string(maximum));
  }
  return result;
}

namespace {

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

}  /* namespace */

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
      options.diagnostic_queue_capacity > 65536 || options.diagnostic_rotation_bytes < 4096 ||
      options.diagnostic_rotation_files < 1 || options.diagnostic_rotation_files > 32) {
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

}  /* namespace context_hmi::options_internal */
