#include "context_hmi/engine.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using context_hmi::DomainError;
using context_hmi::Json;
using namespace context_hmi;

namespace {
Json source(const std::string &id) {
    return Json{{"server", "simulator"}, {"namespace_uri", "urn:test"}, {"identifier", id}};
}
Json model() {
    return Json{
        {"model_id", "pump-station"},
        {"revision", 1},
        {"name", "Pump station"},
        {"assets", Json::array({Json{{"id", "tank-3"},
                                     {"name", "Tank 3"},
                                     {"kind", "tank"},
                                     {"aliases", Json::array({"third tank"})}},
                                Json{{"id", "pump-1"}, {"name", "Pump 1"}, {"kind", "pump"}},
                                Json{{"id", "pump-2"}, {"name", "Pump 2"}, {"kind", "pump"}}})},
        {"relationships",
         Json::array(
             {Json{{"id", "feed-1"}, {"from", "pump-1"}, {"to", "tank-3"}, {"kind", "feeds"}}})},
        {"tags", Json::array({Json{{"id", "tank-3.level"},
                                   {"asset_id", "tank-3"},
                                   {"name", "Level"},
                                   {"role", "level"},
                                   {"data_type", "number"},
                                   {"unit", "%"},
                                   {"source", source("Tank3.Level")}},
                              Json{{"id", "pump-1.flow"},
                                   {"asset_id", "pump-1"},
                                   {"name", "Flow"},
                                   {"role", "flow"},
                                   {"data_type", "number"},
                                   {"unit", "L/s"},
                                   {"source", source("Pump1.Flow")}},
                              Json{{"id", "pump-1.state"},
                                   {"asset_id", "pump-1"},
                                   {"name", "State"},
                                   {"role", "operating_state"},
                                   {"data_type", "boolean"},
                                   {"unit", ""},
                                   {"source", source("Pump1.State")}}})},
        {"alarms", Json::array({Json{{"id", "tank-3.high"},
                                     {"asset_id", "tank-3"},
                                     {"name", "High level"},
                                     {"tag_id", "tank-3.level"},
                                     {"operator", ">="},
                                     {"threshold", 90},
                                     {"severity", "high"}}})}};
}

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    Json m = model();
    check(validate_model(m).at("valid") == true, "valid model accepted");

    Json interpretation = interpret_request(m, "Show filling view for Tank 3");
    check(interpretation.at("status") == "ready", "filling prompt is ready");
    check(interpretation.at("interpreter") == "rules", "rule interpreter is labelled");
    Json view = resolve_task(m, interpretation.at("task"));
    check(view.at("status") == "ready", "declared level and feed produce ready view");
    check(view.at("dependencies").size() == 4,
          "all filling dependencies and scoped alarms are retained");
    check(view.at("model_id") == "pump-station", "view identifies its model");

    Json alarms = resolve_task(m, Json{{"model_id", "pump-station"},
                                       {"kind", "alarms"},
                                       {"model_revision", 1},
                                       {"original_request", "show alarms"}});
    check(alarms.at("components").at(0).at("alarm_id") == "tank-3.high",
          "alarm component identifies alarm");

    Json telemetry = make_telemetry(m, 4, true);
    check(telemetry.at("model_revision") == 1 && telemetry.at("model_id") == "pump-station",
          "telemetry identifies model revision");
    for (const auto &[id, value] : telemetry.at("values").items())
        if (value.at("value").is_number())
            check(value.at("value").get<double>() >= 0.0 &&
                      value.at("value").get<double>() <= 100.0,
                  "simulated number is bounded");

    Json changed = m;
    changed["tags"][0]["data_type"] = "integer";
    check(make_telemetry(changed, 4, true)["values"]["tank-3.level"]["value"].is_number_integer(),
          "integer tags produce integer telemetry values");
    changed = m;
    changed["revision"] = 2;
    changed["tags"][1]["asset_id"] = "pump-2";
    changed["tags"][1]["source"]["identifier"] = "Pump2.FlowAlias";
    Json reconciled = reconcile_view(changed, view);
    check(reconciled.at("status") == "conflict", "same-ID owner/source change is a conflict");
    check(reconciled.at("task").at("original_request") == view.at("task").at("original_request"),
          "reconciliation preserves original task");
    bool saw_conflict = false;
    for (const auto &component : reconciled.at("components"))
        if (component.value("tag_id", "") == "pump-1.flow" &&
            component.value("status", "") == "conflict")
            saw_conflict = true;
    check(saw_conflict, "conflicting component remains blocked");
    Json repeated = reconcile_view(changed, reconciled);
    saw_conflict = false;
    for (const auto &component : repeated.at("components"))
        if (component.value("tag_id", "") == "pump-1.flow" &&
            component.value("status", "") == "conflict")
            saw_conflict = true;
    check(saw_conflict, "conflicting component remains blocked on repeat");

    Json alarm_changed = m;
    alarm_changed["revision"] = 2;
    alarm_changed["alarms"][0]["threshold"] = 80;
    Json alarm_reconciled = reconcile_view(alarm_changed, view);
    saw_conflict = false;
    for (const auto &component : alarm_reconciled.at("components"))
        if (component.value("alarm_id", "") == "tank-3.high" &&
            component.value("status", "") == "conflict")
            saw_conflict = true;
    check(saw_conflict, "changed alarm definition remains blocked");

    Json duplicate_role = m;
    duplicate_role["tags"].push_back(Json{{"id", "tank-3.level.backup"},
                                          {"asset_id", "tank-3"},
                                          {"name", "Level backup"},
                                          {"role", "level"},
                                          {"data_type", "number"},
                                          {"unit", "%"},
                                          {"source", source("Tank3.LevelBackup")}});
    Json duplicate_view = resolve_task(duplicate_role, Json{{"kind", "filling"},
                                                            {"model_id", "pump-station"},
                                                            {"anchor_asset_id", "tank-3"},
                                                            {"model_revision", 1},
                                                            {"original_request", "fill tank 3"}});
    check(duplicate_view.at("status") == "needs-review", "duplicate role is explicit ambiguity");

    Json missing = m;
    missing["revision"] = 2;
    missing["tags"] = Json::array({missing["tags"][1], missing["tags"][2]});
    missing["alarms"] = Json::array();
    Json missing_task = view.at("task");
    missing_task["model_revision"] = 2;
    Json missing_view = resolve_task(missing, missing_task);
    check(missing_view.at("status") == "needs-review", "missing level is explicit");

    Json ambiguous = m;
    ambiguous["assets"].push_back(Json{{"id", "tank-3-replacement"},
                                       {"name", "Tank 3 replacement"},
                                       {"kind", "tank"},
                                       {"aliases", Json::array({"third tank"})}});
    check(interpret_request(ambiguous, "Fill the third tank").at("status") == "clarification",
          "ambiguous alias requests clarification");

    Json invalid = m;
    invalid["relationships"][0]["to"] = "missing";
    bool threw = false;
    try {
        validate_model(invalid);
    } catch (const DomainError &error) {
        threw = error.code == "invalid_model";
    }
    check(threw, "unknown relationship endpoint is rejected");

    std::cout << "engine tests passed\n";
}
