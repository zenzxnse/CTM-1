#include "context_hmi/context_import.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " --bundle PATH [--output PATH]\n"
               "       "
            << program << " --manifest PATH [--output PATH]\n"
               "Writes {model, report} JSON. Output defaults to stdout.\n";
}

}  /* namespace */

int main(int argc, char** argv) {
  std::filesystem::path bundle;
  std::filesystem::path manifest;
  std::filesystem::path output;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    auto value = [&]() -> std::filesystem::path {
      if (index + 1 >= argc) {
        usage(argv[0]);
        std::exit(EXIT_FAILURE);
      }
      ++index;
      return std::filesystem::path(argv[index]);
    };
    if (argument == "--bundle") {
      bundle = value();
    } else if (argument == "--manifest") {
      manifest = value();
    } else if (argument == "--output") {
      output = value();
    } else if (argument == "--help" || argument == "-h") {
      usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if ((!bundle.empty()) == (!manifest.empty())) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  try {
    const auto result = bundle.empty() ? context_hmi::context_import::import_manifest(manifest)
                                      : context_hmi::context_import::import_bundle(bundle);
    const auto document = context_hmi::context_import::Json{{"model", result.model},
                                                             {"report", result.report}};
    if (output.empty()) {
      std::cout << document.dump(2) << '\n';
    } else {
      std::ofstream stream(output, std::ios::binary | std::ios::trunc);
      if (!stream) {
        std::cerr << "Cannot open output: " << output.string() << '\n';
        return EXIT_FAILURE;
      }
      stream << document.dump(2) << '\n';
      if (!stream) {
        std::cerr << "Cannot write output: " << output.string() << '\n';
        return EXIT_FAILURE;
      }
    }
    return result.report.at("errors").empty() ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (const context_hmi::context_import::ImportError& error) {
    std::cerr << error.code << " (" << error.path << "): " << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "import_failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
