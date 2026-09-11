#include "context_hmi/options.hpp"

#include "context_hmi/options_internal.hpp"

namespace context_hmi {

OptionParseResult parse_options(int argc, char** argv) {
  const std::string config_path = options_internal::find_config_path(argc, argv);

  OptionParseResult result;
  if (!config_path.empty()) {
    result.options = options_internal::load_config(config_path);
  }

  options_internal::apply_command_line_overrides(result.options, argc, argv, result.show_help);
  options_internal::validate(result.options);
  return result;
}

}  /* namespace context_hmi */
