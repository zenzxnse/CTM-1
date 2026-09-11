#pragma once

#include <drogon/HttpAppFramework.h>

#include "context_hmi/service_runtime.hpp"

namespace context_hmi::http {

void register_read_routes(drogon::HttpAppFramework& app,
                          service::Runtime& runtime);
void register_task_routes(drogon::HttpAppFramework& app,
                          service::Runtime& runtime);
void register_stream_routes(drogon::HttpAppFramework& app,
                            service::Runtime& runtime);

inline void register_routes(drogon::HttpAppFramework& app,
                            service::Runtime& runtime) {
  register_read_routes(app, runtime);
  register_task_routes(app, runtime);
  register_stream_routes(app, runtime);
}

}  /* namespace context_hmi::http */
