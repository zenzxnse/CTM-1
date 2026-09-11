#include "context_hmi/service_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "context_hmi/engine.hpp"

namespace context_hmi::service {

Json Runtime::catalog() const {
  return Json{{"schema_version", "context-hmi-catalog/1"},
              {"screen_schema_version", "context-hmi-screen/1"},
              {"components",
               Json::array(
                   {Json{{"kind", "gauge"}, {"composer", true}, {"workbench", true}},
                    Json{{"kind", "value"}, {"composer", true}, {"workbench", true}},
                    Json{{"kind", "status"}, {"composer", true}, {"workbench", true}},
                    Json{{"kind", "alarm"}, {"composer", true}, {"workbench", true}},
                    Json{{"kind", "trend"},
                         {"composer", false},
                         {"workbench", true},
                         {"reason", "history contract is not accepted"}},
                    Json{{"kind", "command_button"},
                         {"composer", false},
                         {"workbench", true},
                         {"reason", "visible controls cannot grant command authority"}}})},
              {"machine_inputs",
               Json::array(
                   {Json{{"id", "machine-model-json/1"},
                         {"accepted", true},
                         {"relationships", "declared-only"}},
                    Json{{"id", "engineering-bundle/1"},
                         {"accepted", true},
                         {"importer", "context-hmi-context"},
                         {"relationships", "declared-only"},
                         {"artifacts",
                          Json::array({"asset_hierarchy", "relationships",
                                       "controller_tags", "io_configuration",
                                       "alarm_definitions", "communications",
                                       "machine_documents"})}}})},
              {"clients",
               Json{{"sveltekit", Json{{"implemented", true}, {"primary", true}}},
                    {"qt", Json{{"implemented", false},
                                {"contract_compatible", true},
                                {"reason", "optional renderer is outside the current scope"}}}}},
              {"transport",
               Json{{"http", "drogon-1.9.13"},
                    {"streaming", "server-sent-events"},
                    {"websocket", false}}},
              {"sources", Json::array({"telemetry-ingestion"})}};
}

Json Runtime::telemetry() const {
  auto [model, generation] = model_snapshot();
  return telemetry_store_.snapshot(model, generation, session_id_);
}

Json Runtime::ingest_telemetry(const Json& batch) {
  auto [model, generation] = model_snapshot();
  auto result = telemetry_store_.ingest(model, generation, batch);
  if (generation != model_generation_.load(std::memory_order_acquire)) {
    throw DomainError{"stale_context",
                      "machine context changed while telemetry was committed"};
  }
  source_lifecycle_.transition(lifecycle::State::kReady, "current telemetry received");
  diagnostic_logger_.write(
      diagnostics::Severity::kDebug, "telemetry_batch_accepted",
      Json{{"source_id", result.at("source_id")},
           {"sequence", result.at("sequence")},
           {"value_count", result.at("value_count")},
           {"context_generation", generation}});
  return result;
}

Json Runtime::health() const {
  std::string active_scenario;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(model_mutex_);
    active_scenario = active_scenario_;
    generation = model_generation_.load(std::memory_order_relaxed);
  }
  return Json{{"version", CONTEXT_HMI_VERSION},
              {"http_runtime", "drogon-1.9.13"},
              {"interpreter", inference_.mode() == "rules" ? "rules" : "llama"},
              {"provider_name", inference_.mode()},
              {"llama_configured", !options_.provider_url.empty()},
              {"provider_configured", !options_.provider_url.empty()},
              {"mode", "telemetry-ingestion"},
              {"active_scenario", active_scenario},
              {"session_id", session_id_},
              {"context_generation", generation},
              {"command_gateway", "disabled"},
              {"durable_audit", journal_ != nullptr},
              {"lifecycle",
               Json{{"service", service_lifecycle_.snapshot()},
                    {"source", source_lifecycle_.snapshot()},
                    {"provider", provider_lifecycle_.snapshot()},
                    {"store", store_lifecycle_.snapshot()}}}};
}

Json Runtime::readiness() {
  Json provider = inference_.readiness();
  const bool provider_ready = provider.value("ready", false);
  provider_lifecycle_.transition(provider_ready ? lifecycle::State::kReady
                                                : lifecycle::State::kDegraded,
                                 provider.value("reason", ""));
  Json source = telemetry_store_.readiness(
      model_generation_.load(std::memory_order_acquire));
  const bool source_ready = source.value("ready", false);
  source_lifecycle_.transition(source_ready ? lifecycle::State::kReady
                                            : lifecycle::State::kDegraded,
                               source.value("reason", ""));
  source["lifecycle"] = source_lifecycle_.snapshot();
  return Json{{"ready", provider_ready && source_ready},
              {"service",
               Json{{"ready", service_lifecycle_.state() == lifecycle::State::kReady},
                    {"lifecycle", service_lifecycle_.snapshot()}}},
              {"source", std::move(source)},
              {"inference", std::move(provider)}};
}

Json Runtime::session() const {
  Json capabilities = Json::array({"model.read", "view.create"});
  return Json{{"role", "local-operator"},
              {"actor", options_.operator_actor},
              {"capabilities", std::move(capabilities)},
              {"session_id", session_id_},
              {"context_generation", model_generation_.load(std::memory_order_acquire)},
              {"authentication", "none-loopback-boundary"}};
}

Json Runtime::metrics() const {
  std::size_t saved_views = 0;
  {
    std::lock_guard<std::mutex> lock(view_mutex_);
    saved_views = views_.size();
  }
  Json store = journal_ ? journal_->snapshot() : Json{{"enabled", false}};
  Json source = telemetry_store_.metrics();
  return Json{{"schema_version", "context-hmi-metrics/1"},
              {"execution", cpu_workers_.snapshot()},
              {"provider_execution", provider_workers_.snapshot()},
              {"inference_inflight", inference_inflight_.load(std::memory_order_relaxed)},
              {"sse_clients", sse_clients_.load(std::memory_order_relaxed)},
              {"sse_client_limit", maximum_sse_clients()},
              {"saved_views", saved_views},
              {"cache", Json{{"retrieval", retrieval_cache_.snapshot()},
                              {"interpretation", interpretation_cache_.snapshot()}}},
              {"model_changes", model_change_count_.load(std::memory_order_relaxed)},
              {"model_watch_failures",
               model_watch_failures_.load(std::memory_order_relaxed)},
              {"model_watch_enabled", options_.watch_model},
              {"store", std::move(store)},
              {"diagnostics", diagnostic_logger_.snapshot()},
              {"source", std::move(source)},
              {"lifecycle",
               Json{{"service", service_lifecycle_.snapshot()},
                    {"source", source_lifecycle_.snapshot()},
                    {"provider", provider_lifecycle_.snapshot()},
                    {"store", store_lifecycle_.snapshot()}}}};
}

}  /* namespace context_hmi::service */
