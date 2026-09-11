#include "context_hmi/http_server.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "context_hmi/http_common.hpp"

namespace context_hmi::http {
namespace {

class InferenceAdmission final {
 public:
  explicit InferenceAdmission(service::Runtime& runtime) : runtime_(&runtime) {}
  InferenceAdmission(const InferenceAdmission&) = delete;
  InferenceAdmission& operator=(const InferenceAdmission&) = delete;
  ~InferenceAdmission() { runtime_->release_inference(); }

 private:
  service::Runtime* runtime_;
};

}  /* namespace */

void register_task_routes(drogon::HttpAppFramework& app,
                          service::Runtime& runtime) {
  app.registerHandler(
      "/api/v1/context/retrieve",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(request, callback);
        if (!body) {
          return;
        }
        if (body->size() != 1 || !body->contains("prompt") ||
            !body->at("prompt").is_string()) {
          callback(json_response(
              error_json("invalid_prompt",
                         "request requires only a string prompt"),
              400));
          return;
        }
        const auto prompt = body->at("prompt").get<std::string>();
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, prompt] {
                        return JsonResult{runtime.retrieve(prompt), 200};
                      });
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/interpret",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(request, callback);
        if (!body) {
          return;
        }
        if (!body->contains("prompt") || !body->at("prompt").is_string()) {
          callback(json_response(
              error_json("invalid_prompt", "prompt must be a string"), 400));
          return;
        }
        for (const auto& field : body->items()) {
          if (field.key() != "prompt" && field.key() != "interpreter" &&
              field.key() != "client") {
            callback(json_response(
                error_json("invalid_request",
                           "interpret request contains an unsupported field"),
                400));
            return;
          }
        }
        const auto prompt = body->at("prompt").get<std::string>();
        if (prompt.empty() || prompt.size() > kMaximumPromptBytes) {
          callback(json_response(
              error_json("invalid_prompt",
                         "prompt must be nonempty and at most 8192 bytes"),
              400));
          return;
        }
        std::string mode;
        if (body->contains("interpreter")) {
          if (!body->at("interpreter").is_string()) {
            callback(json_response(error_json(
                                       "invalid_interpreter",
                                       "interpreter must be a string"),
                                   400));
            return;
          }
          mode = body->at("interpreter").get<std::string>();
        }
        Json client = Json::object();
        if (body->contains("client")) {
          if (!body->at("client").is_object()) {
            callback(json_response(
                error_json("invalid_client", "client must be an object"), 400));
            return;
          }
          client = body->at("client");
        }
        if (!runtime.acquire_inference()) {
          callback(json_response(error_json(
                                     "inference_busy",
                                     "inference concurrency limit reached"),
                                 429));
          return;
        }
        auto admission = std::make_shared<InferenceAdmission>(runtime);
        auto& pool = mode == "rules" ||
                             (mode.empty() && runtime.inference_mode() == "rules")
                         ? runtime.cpu_workers()
                         : runtime.provider_workers();
        dispatch_json(pool, std::move(callback),
                      [&runtime, prompt, mode, client,
                       admission = std::move(admission)] {
          (void)admission;
          return JsonResult{runtime.interpret(prompt, mode, client), 200};
        });
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/resolve",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(request, callback);
        if (!body) {
          return;
        }
        if (!body->contains("task") || !body->at("task").is_object()) {
          callback(json_response(
              error_json("invalid_task", "task must be an object"), 400));
          return;
        }
        const auto task = body->at("task");
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, task] {
                        try {
                          return JsonResult{runtime.resolve(task), 200};
                        } catch (const DomainError& error) {
                          throw std::runtime_error(error.details);
                        }
                      });
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/reconcile",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(request, callback);
        if (!body) {
          return;
        }
        Json supplied;
        if (body->contains("view") && body->at("view").is_object()) {
          supplied = body->at("view");
        } else if (body->contains("view_id") && body->size() == 1) {
          supplied = *body;
        } else {
          callback(json_response(
              error_json("invalid_view", "view or view_id is required"), 400));
          return;
        }
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, supplied] {
                        return JsonResult{runtime.reconcile(supplied), 200};
                      });
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/scenario",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(request, callback);
        if (!body) {
          return;
        }
        if (!body->contains("id") || !body->at("id").is_string()) {
          callback(json_response(
              error_json("invalid_scenario", "scenario id is invalid"), 400));
          return;
        }
        const auto id = body->at("id").get<std::string>();
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, id] {
                        return JsonResult{runtime.select_scenario(id), 200};
                      });
      },
      {drogon::Post});
}

}  /* namespace context_hmi::http */
