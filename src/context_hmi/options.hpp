#pragma once

#include <cstdint>
#include <string>

namespace context_hmi {

struct RuntimeOptions {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::string model{"examples/machines/assembly-line.json"};
  std::string models_dir{"examples/machines"};
  bool watch_model{true};
  int model_rescan_ms{500};
  std::uint64_t telemetry_stale_after_ms{3000};
  int telemetry_max_values{512};
  int telemetry_max_batch_bytes{65536};
  std::string web_dir;
  std::string provider_url;
  std::string provider_model{"local-model"};
  std::string provider_api_key;
  std::string provider_api_key_env;
  int provider_timeout_ms{5000};
  int provider_context_bytes{12288};
  int provider_max_output_tokens{256};
  int retrieval_cache_capacity{256};
  int interpretation_cache_capacity{128};
  int cache_ttl_ms{300000};
  bool allow_remote{false};
  bool allow_remote_inference{false};
  std::string workbench_origin;
  std::string store_file;
  bool store_sync{true};
  std::uint64_t store_max_bytes{64U * 1024U * 1024U};
  std::string diagnostic_log_file;
  int diagnostic_queue_capacity{1024};
  std::uint64_t diagnostic_rotation_bytes{8U * 1024U * 1024U};
  int diagnostic_rotation_files{3};
  std::string operator_actor{"local-operator"};
  std::string config_file;
};

struct OptionParseResult {
  RuntimeOptions options;
  bool show_help{false};
};

/** Parse an optional strict JSON configuration, then apply command-line overrides. */
OptionParseResult parse_options(int argc, char** argv);

/** Return the supported command-line interface without exposing configured secrets. */
std::string options_help();

}  /* namespace context_hmi */
