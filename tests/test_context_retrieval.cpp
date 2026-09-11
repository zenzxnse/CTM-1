/*
 * Relevant-context retrieval and semantic validation tests.
 *
 * Retrieval is a bounded evidence step before inference. It may expose declared candidates
 * and direct relationships, but a provider task must remain inside the eligible scope.
 */

#include <string>

#include "context_hmi/context_retrieval.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

using context_hmi::DomainError;
using context_hmi::Json;
using context_hmi::context::RetrievalLimits;
using context_hmi::context::retrieve;
using context_hmi::context::validate_interpretation_scope;
using context_hmi_test::base_model;
using context_hmi_test::check;

namespace {

void an_explicit_asset_retrieves_only_its_relevant_measurements() {
    const Json model = base_model();
    const Json result = retrieve(model, "Show readings for Alpha Tank");
    check(result.at("schema_version") == "context-retrieval/1", "retrieval has a versioned schema");
    check(result.at("eligible_asset_ids") == Json::array({"tank-a"}),
          "the explicit asset is the only eligible identity");
    check(result.at("scope").at("tags").size() == 1,
          "irrelevant assets are excluded from the measurement scope");
    check(result.at("scope").at("tags").at(0).at("asset_id") == "tank-a",
          "every retrieved tag belongs to the selected asset");
    check(result.at("limits").at("tag_limit") == 128, "the configured tag bound is reported");
}

void ambiguity_and_unknown_context_require_clarification() {
    Json model = base_model();
    model["assets"].at(1)["aliases"] = Json::array({"alpha tank"});
    const Json ambiguous = retrieve(model, "Show readings for alpha tank");
    check(ambiguous.at("requires_clarification").get<bool>(),
          "an alias shared by two assets requires clarification");
    check(ambiguous.at("eligible_asset_ids").size() == 2,
          "all equally eligible identities are returned");

    const Json unknown = retrieve(model, "Show readings for an unlisted reactor");
    check(unknown.at("requires_clarification").get<bool>(),
          "an unknown specific asset requires clarification");
    check(unknown.at("eligible_asset_ids").empty(), "unknown context has no eligible identity");
}

void retrieval_limits_are_hard_bounds_and_report_truncation() {
    const Json model = base_model();
    const RetrievalLimits limits{1, 1, 1, 1, 1};
    const Json result = retrieve(model, "show all readings", limits);
    check(result.at("candidates").size() <= 1, "candidate count respects its bound");
    check(result.at("scope").at("assets").size() <= 1, "asset count respects its bound");
    check(result.at("scope").at("relationships").size() <= 1,
          "relationship count respects its bound");
    check(result.at("scope").at("tags").size() <= 1, "tag count respects its bound");
    check(result.at("scope").at("alarms").size() <= 1, "alarm count respects its bound");
    check(result.at("truncated").is_object(), "truncation evidence is returned");
}

void a_provider_cannot_select_another_valid_asset() {
    const Json model = base_model();
    const Json retrieval = retrieve(model, "Show readings for Alpha Tank");
    const Json proposal = Json{{"status", "ready"},
                               {"task", Json{{"kind", "overview"},
                                              {"model_id", model.at("model_id")},
                                              {"model_revision", model.at("revision")},
                                              {"anchor_asset_id", "tank-b"},
                                              {"original_request", "Show readings for Alpha Tank"}}}};
    bool rejected = false;
    try {
        validate_interpretation_scope(model, retrieval, proposal);
    } catch (const DomainError &error) {
        rejected = error.code == "interpretation_mismatch";
    }
    check(rejected, "a valid but out-of-scope asset is rejected as an interpretation mismatch");
}

void a_provider_must_preserve_requested_measurement_roles() {
    const Json model = base_model();
    const Json retrieval = retrieve(model, "Show level readings for Alpha Tank");
    const Json proposal = Json{{"status", "ready"},
                               {"task", Json{{"kind", "overview"},
                                              {"model_id", model.at("model_id")},
                                              {"model_revision", model.at("revision")},
                                              {"anchor_asset_id", "tank-a"},
                                              {"original_request", "Show level readings for Alpha Tank"}}}};
    bool rejected = false;
    try {
        validate_interpretation_scope(model, retrieval, proposal);
    } catch (const DomainError &error) {
        rejected = error.code == "interpretation_mismatch";
    }
    check(rejected, "omitting a retrieved measurement role is rejected");

    Json complete = proposal;
    complete["task"]["measurement_roles"] = Json::array({"level"});
    try {
        validate_interpretation_scope(model, retrieval, complete);
        check(true, "a proposal carrying the requested role is accepted");
    } catch (const DomainError &error) {
        check(false, std::string("a proposal carrying the requested role is accepted: ") +
                         error.what());
    }
}

}  /* namespace */

int main() {
    an_explicit_asset_retrieves_only_its_relevant_measurements();
    ambiguity_and_unknown_context_require_clarification();
    retrieval_limits_are_hard_bounds_and_report_truncation();
    a_provider_cannot_select_another_valid_asset();
    a_provider_must_preserve_requested_measurement_roles();
    return context_hmi_test::report("context retrieval tests");
}
