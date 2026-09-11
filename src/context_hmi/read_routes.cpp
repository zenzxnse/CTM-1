#include "context_hmi/http_server.hpp"

#include <cstddef>
#include <utility>

#include "context_hmi/http_common.hpp"

namespace context_hmi::http {

void register_read_routes(drogon::HttpAppFramework& app,
                          service::Runtime& runtime) {
  app.registerHandler(
      "/api/v1/health",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        callback(json_response(runtime.health()));
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/session",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        callback(json_response(runtime.session()));
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/ready",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        dispatch_json(runtime.provider_workers(), std::move(callback), [&runtime] {
          auto readiness = runtime.readiness();
          return JsonResult{readiness, readiness.value("ready", false) ? 200 : 503};
        });
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/metrics",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        callback(json_response(runtime.metrics()));
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/model",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        callback(json_response(runtime.public_model()));
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/catalog",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        callback(json_response(runtime.catalog()));
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/scenarios",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime] { return JsonResult{runtime.scenarios(), 200}; });
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/audit",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        const auto limit = bounded_limit(request, callback, 50);
        if (!limit) {
          return;
        }
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, limit = *limit] {
                        return JsonResult{runtime.audit(limit), 200};
                      });
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/records",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        const auto limit = bounded_limit(request, callback, 50);
        if (!limit) {
          return;
        }
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, limit = *limit] {
                        return JsonResult{runtime.records(limit), 200};
                      });
      },
      {drogon::Get});
}

}  /* namespace context_hmi::http */
