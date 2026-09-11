#pragma once

#include "context_hmi/options.hpp"

#include <string>

namespace context_hmi::options_internal {

int bounded_integer(const std::string& text, int minimum, int maximum, const char* name);
std::string find_config_path(int argc, char** argv);
RuntimeOptions load_config(const std::string& path);
void apply_command_line_overrides(RuntimeOptions& options, int argc, char** argv,
                                   bool& show_help);
void validate(RuntimeOptions& options);

}  /* namespace context_hmi::options_internal */
