#include "context_hmi/options_internal.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace context_hmi::options_internal {

std::string find_config_path(int argc, char** argv) {
  std::string config_path;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--config") {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value for --config");
      }
      config_path = argv[++index];
    }
  }
  return config_path;
}

void apply_command_line_overrides(RuntimeOptions& options, int argc, char** argv,
                                   bool& show_help) {
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
      options.host = next("--host");
    } else if (argument == "--port") {
      options.port = bounded_integer(next("--port"), 1, 65535, "port");
    } else if (argument == "--model") {
      options.model = next("--model");
    } else if (argument == "--models") {
      options.models_dir = next("--models");
    } else if (argument == "--no-model-watch") {
      options.watch_model = false;
    } else if (argument == "--model-rescan-ms") {
      options.model_rescan_ms = bounded_integer(next("--model-rescan-ms"), 100, 60000,
                                                "model rescan interval");
    } else if (argument == "--telemetry-stale-after-ms") {
      options.telemetry_stale_after_ms = bounded_integer(
          next("--telemetry-stale-after-ms"), 100, 300000, "telemetry stale interval");
    } else if (argument == "--telemetry-max-values") {
      options.telemetry_max_values =
          bounded_integer(next("--telemetry-max-values"), 1, 4096, "telemetry value limit");
    } else if (argument == "--telemetry-max-batch-bytes") {
      options.telemetry_max_batch_bytes = bounded_integer(
          next("--telemetry-max-batch-bytes"), 1024, 1048576, "telemetry batch byte limit");
    } else if (argument == "--web-dir") {
      options.web_dir = next("--web-dir");
    } else if (argument == "--llama-url" || argument == "--provider-url") {
      options.provider_url = next(argument.c_str());
    } else if (argument == "--provider-model") {
      options.provider_model = next("--provider-model");
    } else if (argument == "--provider-context-bytes") {
      options.provider_context_bytes = bounded_integer(next("--provider-context-bytes"), 1024,
                                                       65536, "provider context bytes");
    } else if (argument == "--provider-max-output-tokens") {
      options.provider_max_output_tokens = bounded_integer(
          next("--provider-max-output-tokens"), 64, 2048, "provider maximum output tokens");
    } else if (argument == "--provider-key-env") {
      options.provider_api_key_env = next("--provider-key-env");
    } else if (argument == "--llama-timeout-ms" || argument == "--provider-timeout-ms") {
      options.provider_timeout_ms =
          bounded_integer(next(argument.c_str()), 100, 30000, "provider timeout");
    } else if (argument == "--allow-remote") {
      options.allow_remote = true;
    } else if (argument == "--allow-remote-inference") {
      options.allow_remote_inference = true;
    } else if (argument == "--retrieval-cache-capacity") {
      options.retrieval_cache_capacity = bounded_integer(
          next("--retrieval-cache-capacity"), 1, 4096, "retrieval cache capacity");
    } else if (argument == "--interpretation-cache-capacity") {
      options.interpretation_cache_capacity = bounded_integer(
          next("--interpretation-cache-capacity"), 1, 4096, "interpretation cache capacity");
    } else if (argument == "--cache-ttl-ms") {
      options.cache_ttl_ms =
          bounded_integer(next("--cache-ttl-ms"), 1000, 86400000, "cache TTL");
    } else if (argument == "--workbench-origin") {
      options.workbench_origin = next("--workbench-origin");
    } else if (argument == "--store-file") {
      options.store_file = next("--store-file");
    } else if (argument == "--store-no-sync") {
      options.store_sync = false;
    } else if (argument == "--store-max-bytes") {
      options.store_max_bytes = static_cast<std::uint64_t>(bounded_integer(
          next("--store-max-bytes"), 1024 * 1024, std::numeric_limits<int>::max(),
          "operational store maximum"));
    } else if (argument == "--log-file") {
      options.diagnostic_log_file = next("--log-file");
    } else if (argument == "--log-queue") {
      options.diagnostic_queue_capacity =
          bounded_integer(next("--log-queue"), 16, 65536, "diagnostic queue");
    } else if (argument == "--log-rotation-bytes") {
      options.diagnostic_rotation_bytes = static_cast<std::uint64_t>(bounded_integer(
          next("--log-rotation-bytes"), 4096, std::numeric_limits<int>::max(),
          "diagnostic rotation bytes"));
    } else if (argument == "--log-rotation-files") {
      options.diagnostic_rotation_files =
          bounded_integer(next("--log-rotation-files"), 1, 32, "diagnostic rotation files");
    } else if (argument == "--operator-actor") {
      options.operator_actor = next("--operator-actor");
    } else if (argument == "--help") {
      show_help = true;
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
}

}  /* namespace context_hmi::options_internal */

namespace context_hmi {

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
