#include "inference.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine.hpp"
#include "httplib.h"
#include <nlohmann/json.hpp>

namespace context_hmi {
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
constexpr std::size_t kMaxBody = 64 * 1024;
constexpr std::size_t kMaxPrompt = 8192;

std::string make_session_id() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    return "session-" + std::to_string(now) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string model = "examples/pump-station.json";
    std::string models_dir = "examples";
    std::string web_dir;
    std::string llama_url;
    int llama_timeout_ms = 5000;
    bool allow_remote = false;
    std::string workbench_origin;
};

struct State {
    explicit State(Options o)
        : options(std::move(o)),
          inference(InferenceConfig{options.llama_url,
                                    std::chrono::milliseconds(options.llama_timeout_ms)}),
          active_scenario(fs::path(options.model).stem().string()), session_id(make_session_id()) {}
    Options options;
    std::mutex model_mutex;
    Json model;
    std::atomic<std::uint64_t> tick{0};
    std::atomic<bool> running{true};
    Inference inference;
    std::atomic<unsigned> inference_inflight{0};
    std::atomic<unsigned> sse_clients{0};
    std::atomic<std::uint64_t> model_generation{0};
    std::mutex view_mutex;
    std::unordered_map<std::string, Json> views;
    std::atomic<std::uint64_t> view_sequence{0};
    std::string active_scenario;
    std::string session_id;
};

Json error_json(const std::string &code, const std::string &message) {
    return Json{{"error", Json{{"code", code}, {"message", message}}}};
}

void send_json(httplib::Response &response, const Json &value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json");
}

std::string read_file_bounded(const fs::path &path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 1024 * 1024)
        throw std::runtime_error("model file is missing or too large");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open model file");
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (body.size() > 1024 * 1024)
        throw std::runtime_error("model file is too large");
    return body;
}

bool json_depth_ok(const std::string &input);

Json load_model(const fs::path &path) {
    try {
        const auto body = read_file_bounded(path);
        if (!json_depth_ok(body))
            throw std::runtime_error("model JSON exceeds nesting limit or is incomplete");
        auto value = Json::parse(body);
        validate_model(value);
        return value;
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("invalid model: ") + e.what());
    }
}

Json public_model(State &state) {
    std::lock_guard<std::mutex> lock(state.model_mutex);
    auto model = state.model;
    model["session_id"] = state.session_id;
    model["context_generation"] = state.model_generation.load(std::memory_order_relaxed);
    return model;
}

Json telemetry_snapshot(State &state) {
    std::lock_guard<std::mutex> lock(state.model_mutex);
    const auto &model = state.model;
    auto telemetry = make_telemetry(model, state.tick.load(std::memory_order_relaxed),
                                    state.running.load(std::memory_order_relaxed));
    telemetry["model_id"] = model.at("model_id");
    telemetry["session_id"] = state.session_id;
    telemetry["context_generation"] = state.model_generation.load(std::memory_order_relaxed);
    return telemetry;
}

std::pair<Json, std::uint64_t> snapshot_model_with_generation(State &state) {
    std::lock_guard<std::mutex> lock(state.model_mutex);
    return {state.model, state.model_generation.load(std::memory_order_relaxed)};
}

std::optional<std::string> publish_view(State &state, Json &view,
                                        std::uint64_t expected_generation) {
    std::lock_guard<std::mutex> model_lock(state.model_mutex);
    if (state.model_generation.load(std::memory_order_relaxed) != expected_generation)
        return std::nullopt;
    const auto id =
        state.session_id + "-view-" + std::to_string(state.view_sequence.fetch_add(1) + 1);
    view["view_id"] = id;
    view["session_id"] = state.session_id;
    view["context_generation"] = expected_generation;
    std::lock_guard<std::mutex> lock(state.view_mutex);
    if (state.views.size() >= 64)
        state.views.erase(state.views.begin());
    state.views[id] = view;
    return id;
}

std::optional<Json> trusted_view(State &state, const Json &supplied) {
    if (!supplied.is_object() || !supplied.contains("view_id") ||
        !supplied.at("view_id").is_string())
        return std::nullopt;
    const auto id = supplied.at("view_id").get<std::string>();
    std::lock_guard<std::mutex> lock(state.view_mutex);
    const auto found = state.views.find(id);
    if (found == state.views.end())
        return std::nullopt;
    if (supplied.size() != 1 && supplied != found->second)
        return Json();
    return found->second;
}

bool loopback_host(const std::string &host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool valid_workbench_origin(const std::string &origin) {
    constexpr const char *prefix = "http://";
    if (origin.rfind(prefix, 0) != 0)
        return false;
    const auto authority = origin.substr(7);
    if (authority.empty() || authority.find('/') != std::string::npos ||
        authority.find('@') != std::string::npos)
        return false;
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos)
        return false;
    const auto host = authority.substr(0, colon);
    if (!loopback_host(host))
        return false;
    try {
        const auto port = std::stoi(authority.substr(colon + 1));
        return port > 0 && port <= 65535;
    } catch (...) {
        return false;
    }
}

bool valid_id(const std::string &id) {
    return !id.empty() && id.size() <= 128 &&
           std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]*"));
}

bool json_depth_ok(const std::string &input) {
    std::size_t depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : input) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32)
                return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0 || --depth > 32)
                return false;
        }
    }
    return !quoted && depth == 0;
}

std::optional<Json> parse_body(const httplib::Request &request, httplib::Response &response) {
    if (request.body.size() > kMaxBody) {
        send_json(response, error_json("body_too_large", "request body exceeds maximum size"), 413);
        return std::nullopt;
    }
    if (!json_depth_ok(request.body)) {
        send_json(response,
                  error_json("json_too_deep", "request JSON nesting exceeds maximum depth"), 400);
        return std::nullopt;
    }
    try {
        auto body = Json::parse(request.body);
        if (!body.is_object())
            throw std::runtime_error("request JSON must be an object");
        return body;
    } catch (const std::exception &e) {
        send_json(response, error_json("invalid_json", e.what()), 400);
        return std::nullopt;
    }
}

bool mutation_origin_ok(const httplib::Request &request, const Options &options) {
    if (!request.has_header("Origin"))
        return true;
    const auto origin = request.get_header_value("Origin");
    const auto expected = "http://" + options.host + ":" + std::to_string(options.port);
    const auto local_a = "http://127.0.0.1:" + std::to_string(options.port);
    const auto local_b = "http://localhost:" + std::to_string(options.port);
    return origin == expected || origin == local_a || origin == local_b ||
           (!options.workbench_origin.empty() && origin == options.workbench_origin);
}

void reject_origin(const httplib::Request &request, httplib::Response &response,
                   const Options &options) {
    if (!mutation_origin_ok(request, options))
        send_json(response, error_json("cross_origin", "cross-origin mutation is not permitted"),
                  403);
}

bool mutation_guard(const httplib::Request &request, httplib::Response &response,
                    const Options &options) {
    if (!request.has_header("Content-Type") ||
        request.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
        send_json(response,
                  error_json("content_type", "mutation requests require application/json"), 415);
        return false;
    }
    if (!mutation_origin_ok(request, options)) {
        reject_origin(request, response, options);
        return false;
    }
    return true;
}

void install_routes(httplib::Server &server, State &state) {
    server.new_task_queue = [] { return new httplib::ThreadPool(8, 16); };
    server.set_payload_max_length(kMaxBody);

    server.Get("/api/v1/health", [&](const httplib::Request &, httplib::Response &response) {
        std::string active_scenario;
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(state.model_mutex);
            active_scenario = state.active_scenario;
            generation = state.model_generation.load(std::memory_order_relaxed);
        }
        send_json(response,
                  Json{{"version", "0.1.0"},
                       {"interpreter", state.inference.mode() == "llama.cpp" ? "llama" : "rules"},
                       {"llama_configured", !state.options.llama_url.empty()},
                       {"mode", "simulation"},
                       {"active_scenario", active_scenario},
                       {"session_id", state.session_id},
                       {"context_generation", generation},
                       {"simulation_running", state.running.load(std::memory_order_relaxed)}});
    });

    server.Get("/api/v1/model", [&](const httplib::Request &, httplib::Response &response) {
        send_json(response, public_model(state));
    });

    server.Get("/api/v1/scenarios", [&](const httplib::Request &, httplib::Response &response) {
        Json scenarios = Json::array();
        std::error_code ec;
        if (!fs::is_directory(state.options.models_dir, ec)) {
            send_json(response, Json{{"scenarios", scenarios}});
            return;
        }
        for (const auto &entry : fs::directory_iterator(state.options.models_dir, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json")
                continue;
            try {
                auto model = load_model(entry.path());
                if (!model.is_object() || !model.contains("model_id") ||
                    !model.at("model_id").is_string())
                    continue;
                const auto scenario_id = entry.path().stem().string();
                if (!valid_id(scenario_id))
                    continue;
                const auto scenario_name = model.value(
                    "scenario_name", model.value("name", model.at("model_id").get<std::string>()));
                scenarios.push_back(Json{{"id", scenario_id},
                                         {"model_id", model.at("model_id")},
                                         {"revision", model.at("revision")},
                                         {"name", scenario_name},
                                         {"description", model.value("description", "")}});
            } catch (...) {
                // An invalid scenario is not exposed as a selectable scenario.
            }
        }
        std::sort(scenarios.begin(), scenarios.end(), [](const Json &a, const Json &b) {
            return a.value("id", "") < b.value("id", "");
        });
        send_json(response, Json{{"scenarios", scenarios}});
    });

    server.Post("/api/v1/interpret", [&](const httplib::Request &request,
                                         httplib::Response &response) {
        if (!mutation_guard(request, response, state.options))
            return;
        auto body = parse_body(request, response);
        if (!body)
            return;
        if (!body->contains("prompt") || !body->at("prompt").is_string()) {
            send_json(response, error_json("invalid_prompt", "prompt must be a string"), 400);
            return;
        }
        const auto prompt = body->at("prompt").get<std::string>();
        if (prompt.empty() || prompt.size() > kMaxPrompt) {
            send_json(
                response,
                error_json("invalid_prompt", "prompt must be nonempty and at most 8192 bytes"),
                400);
            return;
        }
        std::string mode;
        if (body->contains("interpreter")) {
            if (!body->at("interpreter").is_string()) {
                send_json(response,
                          error_json("invalid_interpreter", "interpreter must be a string"), 400);
                return;
            }
            mode = body->at("interpreter").get<std::string>();
        }
        unsigned expected = state.inference_inflight.load();
        while (expected < 4 &&
               !state.inference_inflight.compare_exchange_weak(expected, expected + 1)) {
        }
        if (expected >= 4) {
            send_json(response, error_json("inference_busy", "inference concurrency limit reached"),
                      429);
            return;
        }
        try {
            auto [model, generation] = snapshot_model_with_generation(state);
            auto result = state.inference.interpret(model, prompt, mode);
            if (generation != state.model_generation.load(std::memory_order_relaxed))
                throw InferenceError("stale_context",
                                     "active model changed while inference was running");
            if (result.value("status", "") == "ready") {
                if (!result.contains("task") || !result.at("task").is_object())
                    throw InferenceError("provider_schema", "ready interpretation requires task");
                auto &task = result.at("task");
                if (task.contains("model_id") && task.at("model_id") != model.at("model_id"))
                    throw InferenceError("provider_schema",
                                         "provider task model_id does not match active model");
                if (task.contains("model_revision") &&
                    task.at("model_revision") != model.at("revision"))
                    throw InferenceError(
                        "provider_schema",
                        "provider task model_revision does not match active model");
                if (task.contains("anchor_asset_id")) {
                    bool known_asset = false;
                    for (const auto &asset : model.at("assets"))
                        if (asset.value("id", "") ==
                            task.at("anchor_asset_id").get<std::string>()) {
                            known_asset = true;
                            break;
                        }
                    if (!known_asset)
                        throw InferenceError(
                            "provider_schema",
                            "provider task anchor_asset_id is not in the active model");
                }
                task["model_id"] = model.at("model_id");
                task["model_revision"] = model.at("revision");
                auto view = resolve_task(model, task);
                const auto view_id = publish_view(state, view, generation);
                if (!view_id)
                    throw InferenceError("stale_context",
                                         "active model changed while publishing view");
                result["view"] = std::move(view);
            }
            send_json(response, result);
        } catch (const InferenceError &e) {
            send_json(response, error_json(e.code(), e.what()),
                      e.code() == "stale_context"    ? 409
                      : e.code() == "inference_busy" ? 429
                                                     : 502);
        } catch (const std::exception &e) {
            send_json(response, error_json("engine_error", e.what()), 422);
        }
        state.inference_inflight.fetch_sub(1);
    });

    server.Post("/api/v1/resolve", [&](const httplib::Request &request,
                                       httplib::Response &response) {
        if (!mutation_guard(request, response, state.options))
            return;
        auto body = parse_body(request, response);
        if (!body)
            return;
        if (!body->contains("task") || !body->at("task").is_object()) {
            send_json(response, error_json("invalid_task", "task must be an object"), 400);
            return;
        }
        try {
            auto [model, generation] = snapshot_model_with_generation(state);
            auto view = resolve_task(model, body->at("task"));
            const auto view_id = publish_view(state, view, generation);
            if (!view_id) {
                send_json(response,
                          error_json("stale_context", "active model changed while publishing view"),
                          409);
                return;
            }
            send_json(response, view);
        } catch (const std::exception &e) {
            send_json(response, error_json("engine_error", e.what()), 422);
        }
    });

    server.Post(
        "/api/v1/reconcile", [&](const httplib::Request &request, httplib::Response &response) {
            if (!mutation_guard(request, response, state.options))
                return;
            auto body = parse_body(request, response);
            if (!body)
                return;
            const Json *supplied_view = nullptr;
            if (body->contains("view") && body->at("view").is_object())
                supplied_view = &body->at("view");
            else if (body->contains("view_id") && body->size() == 1)
                supplied_view = &*body;
            if (!supplied_view) {
                send_json(response, error_json("invalid_view", "view or view_id is required"), 400);
                return;
            }
            const auto canonical = trusted_view(state, *supplied_view);
            if (!canonical.has_value()) {
                send_json(response,
                          error_json("unknown_view", "view_id is not a known server view"), 422);
                return;
            }
            if (canonical->is_null()) {
                send_json(response,
                          error_json("view_tampered",
                                     "supplied view differs from the trusted server view"),
                          422);
                return;
            }
            try {
                auto [model, generation] = snapshot_model_with_generation(state);
                auto revised = reconcile_view(model, *canonical);
                std::lock_guard<std::mutex> model_lock(state.model_mutex);
                if (state.model_generation.load(std::memory_order_relaxed) != generation) {
                    send_json(
                        response,
                        error_json("stale_context", "active model changed while reconciling view"),
                        409);
                    return;
                }
                revised["view_id"] = canonical->at("view_id");
                revised["session_id"] = state.session_id;
                revised["context_generation"] = generation;
                std::lock_guard<std::mutex> view_lock(state.view_mutex);
                const auto found = state.views.find(revised.at("view_id").get<std::string>());
                if (found == state.views.end() || found->second != *canonical) {
                    send_json(response, error_json("lost_update", "view changed while reconciling"),
                              409);
                    return;
                }
                found->second = revised;
                send_json(response, revised);
            } catch (const std::exception &e) {
                send_json(response, error_json("engine_error", e.what()), 422);
            }
        });

    server.Post(
        "/api/v1/scenario", [&](const httplib::Request &request, httplib::Response &response) {
            if (!mutation_guard(request, response, state.options))
                return;
            auto body = parse_body(request, response);
            if (!body)
                return;
            if (!body->contains("id") || !body->at("id").is_string() ||
                !valid_id(body->at("id").get<std::string>())) {
                send_json(response, error_json("invalid_scenario", "scenario id is invalid"), 400);
                return;
            }
            const auto id = body->at("id").get<std::string>();
            try {
                auto model = load_model(fs::path(state.options.models_dir) / (id + ".json"));
                {
                    std::lock_guard<std::mutex> lock(state.model_mutex);
                    state.model = std::move(model);
                    state.active_scenario = id;
                    state.model_generation.fetch_add(1, std::memory_order_relaxed);
                }
                state.tick.store(0);
                send_json(response, public_model(state));
            } catch (const std::exception &e) {
                send_json(response, error_json("scenario_not_found", e.what()), 404);
            }
        });

    server.Get("/api/v1/telemetry", [&](const httplib::Request &, httplib::Response &response) {
        try {
            send_json(response, telemetry_snapshot(state));
        } catch (const std::exception &e) {
            send_json(response, error_json("telemetry_error", e.what()), 500);
        }
    });

    server.Get("/api/v1/events", [&](const httplib::Request &, httplib::Response &response) {
        unsigned clients = state.sse_clients.load();
        while (clients < 3 && !state.sse_clients.compare_exchange_weak(clients, clients + 1)) {
        }
        if (clients >= 3) {
            send_json(response, error_json("events_busy", "SSE client limit reached"), 429);
            return;
        }
        response.set_chunked_content_provider(
            "text/event-stream",
            [&](std::size_t, httplib::DataSink &sink) {
                while (sink.is_writable()) {
                    try {
                        const auto payload = telemetry_snapshot(state).dump();
                        const auto event = "event: telemetry\ndata: " + payload + "\n\n";
                        if (!sink.write(event.data(), event.size()))
                            break;
                    } catch (...) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                sink.done();
                return false;
            },
            [&](bool) { state.sse_clients.fetch_sub(1); });
        response.set_header("Cache-Control", "no-cache");
        response.set_header("Connection", "keep-alive");
    });

    server.Post("/api/v1/simulation", [&](const httplib::Request &request,
                                          httplib::Response &response) {
        if (!mutation_guard(request, response, state.options))
            return;
        auto body = parse_body(request, response);
        if (!body)
            return;
        if (!body->contains("running") || !body->at("running").is_boolean()) {
            send_json(response, error_json("invalid_simulation", "running must be boolean"), 400);
            return;
        }
        state.running.store(body->at("running").get<bool>());
        send_json(response, Json{{"running", state.running.load()}});
    });

    server.set_error_handler([](const httplib::Request &, httplib::Response &response) {
        if (response.body.empty())
            send_json(response, error_json("not_found", "route not found"),
                      response.status == 404 ? 404 : response.status);
    });
}

Options parse_options(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--host")
            options.host = next("--host");
        else if (arg == "--port")
            options.port = std::stoi(next("--port"));
        else if (arg == "--model")
            options.model = next("--model");
        else if (arg == "--models")
            options.models_dir = next("--models");
        else if (arg == "--web-dir")
            options.web_dir = next("--web-dir");
        else if (arg == "--llama-url")
            options.llama_url = next("--llama-url");
        else if (arg == "--llama-timeout-ms")
            options.llama_timeout_ms = std::stoi(next("--llama-timeout-ms"));
        else if (arg == "--allow-remote")
            options.allow_remote = true;
        else if (arg == "--workbench-origin")
            options.workbench_origin = next("--workbench-origin");
        else if (arg == "--help") {
            std::cout << "context-hmi [--host HOST] [--port PORT] [--model FILE] [--models "
                         "DIR] [--web-dir DIR] [--llama-url URL] [--llama-timeout-ms MS] "
                         "[--workbench-origin http://127.0.0.1:5173] [--allow-remote]\n";
            std::exit(0);
        } else
            throw std::runtime_error("unknown option: " + arg);
    }
    if (options.port < 1 || options.port > 65535)
        throw std::runtime_error("port out of range");
    if (options.llama_timeout_ms < 100 || options.llama_timeout_ms > 30000)
        throw std::runtime_error("llama timeout must be between 100 and 30000 ms");
    if (!options.workbench_origin.empty() && !valid_workbench_origin(options.workbench_origin))
        throw std::runtime_error(
            "--workbench-origin must be an http:// loopback origin with a port");
    if (!options.allow_remote && !loopback_host(options.host))
        throw std::runtime_error("non-loopback bind requires --allow-remote");
    return options;
}

} // namespace
} // namespace context_hmi

int main(int argc, char **argv) {
    using namespace context_hmi;
    try {
        auto options = parse_options(argc, argv);
        State state(options);
        state.model = load_model(options.model);
        httplib::Server server;
        install_routes(server, state);
        if (!options.web_dir.empty()) {
            if (!server.set_mount_point("/", options.web_dir))
                throw std::runtime_error("web directory does not exist");
        }
        std::jthread ticker([&state](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (state.running.load(std::memory_order_relaxed))
                    state.tick.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::cerr << "context-hmi listening on http://" << options.host << ":" << options.port
                  << "\n";
        if (!server.listen(options.host, options.port))
            throw std::runtime_error("failed to listen");
    } catch (const std::exception &e) {
        std::cerr << "context-hmi: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
