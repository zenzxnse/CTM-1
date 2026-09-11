#include <exception>
#include <iostream>
#include <utility>

#include <drogon/drogon.h>

#include "context_hmi/http_common.hpp"
#include "context_hmi/http_server.hpp"
#include "context_hmi/options.hpp"
#include "context_hmi/service_runtime.hpp"

int main(int argc, char** argv) {
  try {
    auto parsed = context_hmi::parse_options(argc, argv);
    if (parsed.show_help) {
      std::cout << context_hmi::options_help();
      return 0;
    }
    context_hmi::service::Runtime runtime(std::move(parsed.options));
    runtime.start();
    auto& application = drogon::app();
    context_hmi::http::configure_framework(application, runtime.options());
    context_hmi::http::register_routes(application, runtime);
    std::cerr << "context-hmi listening on http://" << runtime.options().host << ':'
              << runtime.options().port << " with Drogon 1.9.13\n";
    application.run();
    runtime.shutdown();
  } catch (const std::exception& error) {
    std::cerr << "context-hmi: " << error.what() << '\n';
    return 2;
  }
  return 0;
}
