#include "context_hmi/engine.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>

using context_hmi::DomainError;
using context_hmi::Json;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

template <typename Fn> void expects_domain_error(Fn &&fn, const std::string &message) {
    try {
        fn();
        check(false, message + " (no DomainError)");
    } catch (const DomainError &) {
        /* Expected. */
    } catch (const std::exception &error) {
        check(false, message + " (wrong exception: " + error.what() + ")");
    }
}

Json source(const std::string &identifier) {
    return Json{{"server", "external-source"},
                {"namespace_uri", "urn:context-hmi:source"},
                {"identifier", identifier}};
}

Json tag(const std::string &id, const std::string &owner, const std::string &name,
         const std::string &role, const std::string &type, const std::string &unit,
         const std::string &identifier) {
    return Json{{"id", id},
                {"asset_id", owner},
                {"name", name},
                {"role", role},
                {"data_type", type},
                {"unit", unit},
                {"source", source(identifier)}};
}

Json base_model() {
    return Json{
        {"model_id", "m1"},
        {"revision", 1},
        {"name", "Station"},
        {"assets", Json::array({
                       Json{{"id", "tank-a"},
                            {"name", "Tank A"},
                            {"kind", "tank"},
                            {"aliases", Json::array({"alpha tank"})}},
                       Json{{"id", "tank-b"}, {"name", "Tank B"}, {"kind", "tank"}},
                       Json{{"id", "pump-a"}, {"name", "Pump A"}, {"kind", "pump"}},
                       Json{{"id", "pump-b"}, {"name", "Pump B"}, {"kind", "pump"}},
                   })},
        {"relationships",
         Json::array(
             {Json{{"id", "feed-1"}, {"from", "pump-a"}, {"to", "tank-a"}, {"kind", "feeds"}}})},
        {"tags", Json::array({
                     tag("tank-a.level", "tank-a", "Level", "level", "number", "%", "TankA.Level"),
                     tag("tank-a.pressure", "tank-a", "Pressure", "pressure", "number", "bar",
                         "TankA.Pressure"),
                     tag("pump-a.flow", "pump-a", "Flow", "flow", "number", "m3/h", "PumpA.Flow"),
                     tag("pump-a.state", "pump-a", "State", "operating_state", "string", "state",
                         "PumpA.State"),
                     tag("pump-b.flow", "pump-b", "Flow", "flow", "number", "m3/h", "PumpB.Flow"),
                     tag("pump-b.state", "pump-b", "State", "operating_state", "string", "state",
                         "PumpB.State"),
                 })},
        {"alarms", Json::array({Json{{"id", "alarm-a"},
                                     {"asset_id", "tank-a"},
                                     {"name", "High level"},
                                     {"tag_id", "tank-a.level"},
                                     {"operator", ">="},
                                     {"threshold", 90},
                                     {"severity", "high"}}})},
    };
}

Json overview_view(const Json &model) {
    return context_hmi::resolve_task(model, Json{{"kind", "overview"},
                                                 {"anchor_asset_id", "tank-a"},
                                                 {"model_revision", 1},
                                                 {"original_request", "Show overview for Tank A"}});
}

void same_id_tag_changes_are_conflicts() {
    const Json original = base_model();
    const Json view = overview_view(original);

    Json owner_changed = original;
    owner_changed["tags"][0]["asset_id"] = "tank-b";
    owner_changed["alarms"][0]["asset_id"] = "tank-b";
    auto reconciled = context_hmi::reconcile_view(owner_changed, view);
    check(reconciled["status"] == "conflict", "same-ID owner change is a conflict");
    check(std::any_of(reconciled["changes"].begin(), reconciled["changes"].end(),
                      [](const Json &change) { return change.value("kind", "") == "conflict"; }),
          "same-ID owner change is reported as a conflict");

    Json role_changed = original;
    role_changed["tags"][0]["role"] = "pressure";
    reconciled = context_hmi::reconcile_view(role_changed, view);
    check(reconciled["status"] == "conflict", "same-ID role change is a conflict");

    Json unit_changed = original;
    unit_changed["tags"][0]["unit"] = "litres";
    reconciled = context_hmi::reconcile_view(unit_changed, view);
    check(reconciled["status"] == "conflict", "same-ID unit change is a conflict");
}

void same_id_relationship_changes_are_conflicts() {
    const Json original = base_model();
    Json view = overview_view(original);
    view["components"][0]["relationship_id"] = "feed-1";
    view["components"][0]["relationship_kind"] = "feeds";
    view["components"][0]["relationship_from"] = "pump-a";
    view["components"][0]["relationship_to"] = "tank-a";
    Json changed = original;
    changed["relationships"][0]["from"] = "pump-b";
    auto reconciled = context_hmi::reconcile_view(changed, view);
    check(reconciled["status"] == "conflict", "same-ID relationship endpoint change is a conflict");
    check(std::any_of(reconciled["changes"].begin(), reconciled["changes"].end(),
                      [](const Json &change) {
                          return change.value("kind", "").find("relationship") !=
                                     std::string::npos ||
                                 change.value("kind", "") == "conflict";
                      }),
          "same-ID relationship endpoint change is reported");
}

void repeated_reconcile_does_not_launder_conflict() {
    const Json original = base_model();
    const Json view = overview_view(original);
    Json changed = original;
    changed["tags"][0]["unit"] = "litres";
    const Json once = context_hmi::reconcile_view(changed, view);
    const Json twice = context_hmi::reconcile_view(changed, once);
    check(once["status"] == "conflict" && twice["status"] == "conflict",
          "a same-ID conflict remains conflict on repeated reconciliation");
    check(std::any_of(twice["changes"].begin(), twice["changes"].end(),
                      [](const Json &change) { return change.value("kind", "") == "conflict"; }),
          "a same-ID conflict is not laundered by a second reconciliation");
}

void model_and_task_identity_are_bound() {
    const Json original = base_model();
    const Json view = overview_view(original);
    Json different_model = original;
    different_model["model_id"] = "m2";
    bool model_rejected = false;
    try {
        const Json reconciled = context_hmi::reconcile_view(different_model, view);
        model_rejected =
            reconciled["status"] == "needs-review" || reconciled["status"] == "conflict";
    } catch (const DomainError &error) {
        model_rejected = error.code == "model_mismatch";
    }
    check(model_rejected, "a different model with the same revision cannot silently reconcile");

    Json stale_task = Json{{"kind", "overview"},
                           {"anchor_asset_id", "tank-a"},
                           {"model_revision", 999},
                           {"original_request", "Show overview for Tank A"}};
    expects_domain_error([&] { (void)context_hmi::resolve_task(original, stale_task); },
                         "a task from another revision is rejected");
}

void malformed_models_and_ids_are_rejected() {
    Json malformed = base_model();
    malformed["revision"] = "1";
    expects_domain_error([&] { (void)context_hmi::validate_model(malformed); },
                         "revision with wrong JSON type is rejected");

    malformed = base_model();
    malformed["tags"][0]["unit"] = 7;
    expects_domain_error([&] { (void)context_hmi::validate_model(malformed); },
                         "tag unit with wrong JSON type is rejected");

    malformed = base_model();
    malformed["assets"].push_back(malformed["assets"][0]);
    expects_domain_error([&] { (void)context_hmi::validate_model(malformed); },
                         "duplicate asset IDs are rejected");

    malformed = base_model();
    malformed["tags"].push_back(malformed["tags"][0]);
    expects_domain_error([&] { (void)context_hmi::validate_model(malformed); },
                         "duplicate tag IDs are rejected");

    malformed = base_model();
    malformed["assets"] = Json::array();
    for (int i = 0; i < 257; ++i)
        malformed["assets"].push_back(
            Json{{"id", "asset-" + std::to_string(i)}, {"name", "Asset"}, {"kind", "tank"}});
    expects_domain_error([&] { (void)context_hmi::validate_model(malformed); },
                         "asset cardinality limit is enforced");
}

void client_dependencies_are_not_authority() {
    const Json model = base_model();
    Json forged = overview_view(model);
    check(forged.contains("dependencies") && !forged["dependencies"].empty(),
          "fixture has a dependency");
    forged["dependencies"][0]["owner"] = "attacker-asset";
    forged["dependencies"][0].erase("fingerprint");
    const Json reconciled = context_hmi::reconcile_view(model, forged);
    check((reconciled["status"] == "conflict" || reconciled["status"] == "needs-review") &&
              !reconciled["changes"].empty(),
          "forged dependency metadata is detected instead of trusted");
}

}  /* namespace */

int main() {
    same_id_tag_changes_are_conflicts();
    same_id_relationship_changes_are_conflicts();
    repeated_reconcile_does_not_launder_conflict();
    model_and_task_identity_are_bound();
    malformed_models_and_ids_are_rejected();
    client_dependencies_are_not_authority();
    if (failures != 0)
        std::cerr << failures << " adversarial checks failed\n";
    return failures == 0 ? 0 : 1;
}
