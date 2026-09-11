#include "context_hmi/http_server.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include <trantor/net/EventLoop.h>

#include "context_hmi/http_common.hpp"

namespace context_hmi::http {
namespace {

class StreamPump final : public std::enable_shared_from_this<StreamPump> {
 public:
  StreamPump(service::Runtime& runtime, trantor::EventLoop* loop,
             drogon::ResponseStreamPtr stream)
      : runtime_(&runtime), loop_(loop), stream_(std::move(stream)) {}

  StreamPump(const StreamPump&) = delete;
  StreamPump& operator=(const StreamPump&) = delete;

  ~StreamPump() {
    if (stream_) {
      stream_->close();
    }
    release();
  }

  void start() {
    if (!send_snapshot()) {
      stop();
      return;
    }
    auto self = shared_from_this();
    timer_ = loop_->runEvery(0.25, [self = std::move(self)] {
      if (!self->send_snapshot()) {
        self->stop();
      }
    });
  }

 private:
  bool send_snapshot() {
    if (closed_.load(std::memory_order_acquire) || !stream_) {
      return false;
    }
    try {
      const auto event = std::string("event: telemetry\ndata: ") +
                         runtime_->telemetry().dump() + "\n\n";
      return stream_->send(event);
    } catch (...) {
      return false;
    }
  }

  void stop() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (timer_ != trantor::InvalidTimerId) {
      loop_->invalidateTimer(timer_);
      timer_ = trantor::InvalidTimerId;
    }
    if (stream_) {
      stream_->close();
      stream_.reset();
    }
    release();
  }

  void release() {
    if (!released_.exchange(true, std::memory_order_acq_rel)) {
      runtime_->release_sse_client();
    }
  }

  service::Runtime* runtime_;
  trantor::EventLoop* loop_;
  drogon::ResponseStreamPtr stream_;
  trantor::TimerId timer_{trantor::InvalidTimerId};
  std::atomic<bool> closed_{false};
  std::atomic<bool> released_{false};
};

}  /* namespace */

void register_stream_routes(drogon::HttpAppFramework& app,
                            service::Runtime& runtime) {
  app.registerHandler(
      "/api/v1/telemetry",
      [&runtime](const drogon::HttpRequestPtr&, ResponseCallback&& callback) {
        try {
          callback(json_response(runtime.telemetry()));
        } catch (const std::exception& error) {
          callback(json_response(error_json("telemetry_error", error.what()), 500));
        }
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/telemetry",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        if (!mutation_guard(request, callback, runtime.options())) {
          return;
        }
        const auto body = parse_body(
            request, callback,
            static_cast<std::size_t>(runtime.options().telemetry_max_batch_bytes));
        if (!body) {
          return;
        }
        dispatch_json(runtime.cpu_workers(), std::move(callback),
                      [&runtime, batch = *body] {
                        return JsonResult{runtime.ingest_telemetry(batch), 202};
                      });
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/events",
      [&runtime](const drogon::HttpRequestPtr& request,
                 ResponseCallback&& callback) {
        const auto& connection = request->getHeader("connection");
        const bool closes_response =
            connection == "close" ||
            (request->getVersion() == drogon::Version::kHttp10 &&
             connection != "keep-alive");
        if (closes_response) {
          try {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeString("text/event-stream");
            response->addHeader("Cache-Control", "no-cache");
            response->addHeader("X-Accel-Buffering", "no");
            response->setBody(std::string("event: telemetry\ndata: ") +
                              runtime.telemetry().dump() + "\n\n");
            callback(response);
          } catch (const std::exception& error) {
            callback(json_response(
                error_json("telemetry_error", error.what()), 500));
          }
          return;
        }
        if (!runtime.acquire_sse_client()) {
          callback(json_response(
              error_json("events_busy", "SSE client limit reached"), 429));
          return;
        }
        auto response = drogon::HttpResponse::newAsyncStreamResponse(
            [&runtime](drogon::ResponseStreamPtr stream) {
              auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
              if (loop == nullptr) {
                loop = drogon::app().getLoop();
              }
              auto pump = std::make_shared<StreamPump>(
                  runtime, loop, std::move(stream));
              loop->queueInLoop([pump = std::move(pump)] { pump->start(); });
            },
            true);
        response->setContentTypeString("text/event-stream");
        response->addHeader("Cache-Control", "no-cache");
        response->addHeader("Connection", "keep-alive");
        response->addHeader("X-Accel-Buffering", "no");
        callback(response);
      },
      {drogon::Get});
}

}  /* namespace context_hmi::http */
