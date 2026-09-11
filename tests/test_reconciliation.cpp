/*
 * Revision-aware view reconciliation tests.
 *
 * A saved view is evidence from an earlier model revision. Reconciliation may add newly
 * declared bindings, but it must preserve removed or same-ID semantic changes as explicit
 * unavailable or conflict states.
 */

#include <string>

#include "context_hmi/engine.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

using context_hmi::DomainError;
using context_hmi::Json;
using context_hmi::reconcile_view;
using context_hmi_test::base_model;
using context_hmi_test::check;
using context_hmi_test::find_by;
using context_hmi_test::make_tag;
using context_hmi_test::overview_view;
using context_hmi_test::string_or_empty;

namespace {

void expect_component_status(const Json &result, const std::string &component_id,
                             const std::string &status, const std::string &message) {
    const Json *component = find_by(result.at("components"), "id", component_id);
    if (component == nullptr) {
        check(false, message + " (component " + component_id + " is missing)");
        return;
    }
    check(string_or_empty(*component, "status") == status,
          message + " (expected status " + status + ")");
}

void unchanged_views_reconcile_without_changes() {
    const Json model = base_model();
    const Json view = overview_view(model);
    const Json result = reconcile_view(model, view);
    check(result.at("status") == "ready", "an unchanged model remains ready");
    check(result.at("reconciled") == true, "the result records reconciliation");
    check(result.at("changes").empty(), "an unchanged model has no changes");
    check(result.at("issues").empty(), "an unchanged model has no issues");
}

void same_id_semantic_changes_are_conflicts() {
    const Json original = base_model();
    const Json view = overview_view(original);
    Json changed = original;
    changed["revision"] = 2;
    changed["tags"][0]["role"] = "pressure";
    const Json role_result = reconcile_view(changed, view);
    check(role_result.at("status") == "conflict", "a role change is a conflict");
    expect_component_status(role_result, "tank-a.level", "conflict",
                            "a role change blocks the old binding");

    changed = original;
    changed["revision"] = 2;
    changed["tags"][0]["unit"] = "litres";
    const Json unit_result = reconcile_view(changed, view);
    check(unit_result.at("status") == "conflict", "a unit change is a conflict");
    expect_component_status(unit_result, "tank-a.level", "conflict",
                            "a unit change blocks the old binding");
}

void removed_bindings_become_unavailable() {
    const Json original = base_model();
    const Json view = overview_view(original);
    Json changed = original;
    changed["revision"] = 2;
    changed["tags"] = Json::array({changed["tags"][1], changed["tags"][2], changed["tags"][3],
                                    changed["tags"][4]});
    changed["alarms"] = Json::array();
    const Json result = reconcile_view(changed, view);
    check(result.at("status") == "needs-review", "a removed binding needs review");
    expect_component_status(result, "tank-a.level", "unavailable",
                            "a removed binding is not rebound");
}

void client_dependency_metadata_is_not_authority() {
    const Json model = base_model();
    Json forged = overview_view(model);
    forged["dependencies"][0]["owner"] = "attacker-asset";
    forged["dependencies"][0].erase("fingerprint");
    const Json result = reconcile_view(model, forged);
    check(result.at("status") == "conflict" || result.at("status") == "needs-review",
          "forged dependency metadata is detected");
    check(!result.at("changes").empty(), "dependency tampering produces an explanation");
}

void model_identity_cannot_be_replaced_by_revision() {
    const Json model = base_model();
    const Json view = overview_view(model);
    Json foreign = model;
    foreign["model_id"] = "foreign-model";
    bool rejected = false;
    try {
        const Json result = reconcile_view(foreign, view);
        rejected = result.at("status") == "conflict" || result.at("status") == "needs-review";
    } catch (const DomainError &error) {
        rejected = error.code == "model_mismatch";
    }
    check(rejected, "a foreign model cannot reconcile by revision alone");
}

void newly_declared_scoped_tags_are_added_without_rebinding_old_tags() {
    const Json original = base_model();
    const Json view = overview_view(original);
    Json changed = original;
    changed["revision"] = 2;
    changed["tags"].push_back(make_tag("tank-a.temperature", "tank-a", "Temperature",
                                       "temperature", "number", "degC", "TankA.Temperature"));
    const Json result = reconcile_view(changed, view);
    check(result.at("status") == "ready", "a compatible declaration keeps the view ready");
    check(find_by(result.at("components"), "id", "tank-a.level") != nullptr,
          "the original binding remains present");
    check(find_by(result.at("components"), "id", "tank-a.temperature") != nullptr,
          "a newly declared scoped binding is available");
}

}  /* namespace */

int main() {
    unchanged_views_reconcile_without_changes();
    same_id_semantic_changes_are_conflicts();
    removed_bindings_become_unavailable();
    client_dependency_metadata_is_not_authority();
    model_identity_cannot_be_replaced_by_revision();
    newly_declared_scoped_tags_are_added_without_rebinding_old_tags();
    return context_hmi_test::report("reconciliation tests");
}
