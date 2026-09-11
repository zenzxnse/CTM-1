/**
 * Model validation boundaries and identity rules.
 *
 * Contract under test: validate_model enforces declared identity, reference resolution,
 * field types, cardinality and alarm semantics before any interpretation or telemetry
 * work. Every rejection is a DomainError with code "invalid_model". Two identities that
 * differ only by a confusable Unicode character remain two identities.
 */

#include <cmath>
#include <limits>
#include <string>

#include "context_hmi/engine.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

using context_hmi::Json;
using context_hmi::validate_model;
using context_hmi_test::base_model;
using context_hmi_test::check;
using context_hmi_test::expects_domain_error;
using context_hmi_test::expects_no_throw;
using context_hmi_test::make_asset;
using context_hmi_test::make_tag;

namespace {

/** U+0430 CYRILLIC SMALL LETTER A, written as explicit UTF-8 bytes for portability. */
const char *const kCyrillicSmallA = "\xd0\xb0";
/** U+0410 CYRILLIC CAPITAL LETTER A. */
const char *const kCyrillicCapitalA = "\xd0\x90";

void rejects(const Json &model, const std::string &message) {
    expects_domain_error([&] { (void)validate_model(model); }, "invalid_model", message);
}

void accepted_model_reports_declared_counts() {
    const Json model = base_model();
    Json summary;
    expects_no_throw([&] { summary = validate_model(model); }, "the base fixture validates");
    check(summary.at("valid") == true, "a valid model reports valid true");
    check(summary.at("model_id") == "station-a", "the summary echoes the declared model id");
    check(summary.at("revision") == 1, "the summary echoes the declared revision");
    check(summary.at("asset_count") == 4, "the summary counts declared assets");
    check(summary.at("relationship_count") == 1, "the summary counts declared relationships");
    check(summary.at("tag_count") == 5, "the summary counts declared tags");
    check(summary.at("alarm_count") == 1, "the summary counts declared alarms");
}

void model_envelope_fields_are_bounded() {
    rejects(Json::array(), "a model that is not an object is rejected");

    Json model = base_model();
    model.erase("model_id");
    rejects(model, "a model without model_id is rejected");

    model = base_model();
    model["model_id"] = "";
    rejects(model, "an empty model_id is rejected");

    model = base_model();
    model["model_id"] = std::string(257, 'x');
    rejects(model, "a model_id above the 256 character limit is rejected");

    model = base_model();
    model["revision"] = "1";
    rejects(model, "a revision declared as a string is rejected");

    model = base_model();
    model["revision"] = -1;
    rejects(model, "a negative revision is rejected");

    model = base_model();
    model["revision"] = 4294967296LL;
    rejects(model, "a revision above the 32 bit range is rejected");

    model = base_model();
    model["revision"] = 4294967295ULL;
    expects_no_throw([&] { (void)validate_model(model); },
                     "the highest 32 bit revision is accepted");

    model = base_model();
    model["assets"] = Json::object();
    rejects(model, "an assets field that is not an array is rejected");

    model = base_model();
    model.erase("tags");
    rejects(model, "a model without a tags array is rejected");
}

Json model_with_asset_count(std::size_t count) {
    Json model{{"model_id", "bulk"},
               {"revision", 1},
               {"name", "Bulk"},
               {"assets", Json::array()},
               {"relationships", Json::array()},
               {"tags", Json::array()},
               {"alarms", Json::array()}};
    for (std::size_t i = 0; i < count; ++i)
        model["assets"].push_back(make_asset("asset-" + std::to_string(i), "Asset", "tank"));
    return model;
}

void cardinality_limits_are_enforced() {
    expects_no_throw([&] { (void)validate_model(model_with_asset_count(256)); },
                     "exactly 256 assets are accepted");
    rejects(model_with_asset_count(257), "257 assets exceed the declared asset limit");

    Json model = model_with_asset_count(1);
    for (std::size_t i = 0; i < 513; ++i)
        model["relationships"].push_back(Json{{"id", "rel-" + std::to_string(i)},
                                              {"from", "asset-0"},
                                              {"to", "asset-0"},
                                              {"kind", "feeds"}});
    rejects(model, "513 relationships exceed the declared relationship limit");

    model = model_with_asset_count(1);
    for (std::size_t i = 0; i < 2049; ++i)
        model["tags"].push_back(make_tag("tag-" + std::to_string(i), "asset-0", "Value", "flow",
                                         "number", "m3/h", "Bulk.Value"));
    rejects(model, "2049 tags exceed the declared tag limit");

    model = model_with_asset_count(1);
    model["tags"].push_back(
        make_tag("tag-0", "asset-0", "Value", "flow", "number", "m3/h", "Bulk.Value"));
    for (std::size_t i = 0; i < 2049; ++i)
        model["alarms"].push_back(Json{{"id", "alarm-" + std::to_string(i)},
                                       {"asset_id", "asset-0"},
                                       {"name", "High"},
                                       {"tag_id", "tag-0"},
                                       {"operator", ">"},
                                       {"threshold", 1},
                                       {"severity", "low"}});
    rejects(model, "2049 alarms exceed the declared alarm limit");
}

void alias_limits_are_enforced() {
    Json model = base_model();
    model["assets"][0]["aliases"] = "alpha tank";
    rejects(model, "an aliases field that is not an array is rejected");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array();
    for (std::size_t i = 0; i < 33; ++i)
        model["assets"][0]["aliases"].push_back("alias-" + std::to_string(i));
    rejects(model, "33 aliases exceed the declared alias limit");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array();
    for (std::size_t i = 0; i < 32; ++i)
        model["assets"][0]["aliases"].push_back("alias-" + std::to_string(i));
    expects_no_throw([&] { (void)validate_model(model); }, "exactly 32 aliases are accepted");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array({""});
    rejects(model, "an empty alias is rejected");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array({std::string(257, 'x')});
    rejects(model, "an alias above the 256 character limit is rejected");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array({7});
    rejects(model, "a non-string alias is rejected");
}

void duplicate_identities_are_rejected() {
    Json model = base_model();
    model["assets"].push_back(model["assets"][0]);
    rejects(model, "a duplicate asset id is rejected");

    model = base_model();
    model["tags"].push_back(model["tags"][0]);
    rejects(model, "a duplicate tag id is rejected");

    model = base_model();
    model["alarms"].push_back(model["alarms"][0]);
    rejects(model, "a duplicate alarm id is rejected");

    model = base_model();
    model["relationships"].push_back(model["relationships"][0]);
    rejects(model, "a duplicate relationship id is rejected");
}

void relationships_must_resolve_to_declared_assets() {
    Json model = base_model();
    model["relationships"][0]["from"] = "pump-unknown";
    rejects(model, "a relationship from an undeclared asset is rejected");

    model = base_model();
    model["relationships"][0]["to"] = "tank-unknown";
    rejects(model, "a relationship to an undeclared asset is rejected");

    model = base_model();
    model["relationships"][0]["to"] = model["relationships"][0]["from"];
    rejects(model, "a relationship from an asset to itself is rejected");

    model = base_model();
    model["relationships"][0].erase("kind");
    rejects(model, "a relationship without a declared kind is rejected");
}

void tag_declarations_are_bounded() {
    Json model = base_model();
    model["tags"][0]["asset_id"] = "tank-unknown";
    rejects(model, "a tag owned by an undeclared asset is rejected");

    model = base_model();
    model["tags"][0]["data_type"] = "json";
    rejects(model, "an unsupported data_type is rejected");

    model = base_model();
    model["tags"][0]["data_type"] = "NUMBER";
    expects_no_throw([&] { (void)validate_model(model); }, "data_type comparison ignores case");

    model = base_model();
    model["tags"][0]["data_type"] = "string";
    rejects(model, "a level role declared as string is rejected");

    model = base_model();
    model["tags"][1]["data_type"] = "boolean";
    rejects(model, "a flow role declared as boolean is rejected");

    model = base_model();
    model["tags"][0]["role"] = "LEVEL";
    expects_no_throw([&] { (void)validate_model(model); }, "role comparison ignores case");

    model = base_model();
    model["tags"][0]["unit"] = 7;
    rejects(model, "a non-string unit is rejected");

    model = base_model();
    model["tags"][0].erase("unit");
    rejects(model, "a tag without a unit field is rejected");

    model = base_model();
    model["tags"][0]["unit"] = "";
    expects_no_throw([&] { (void)validate_model(model); },
                     "an empty unit is accepted as declared metadata");

    model = base_model();
    model["tags"][0].erase("name");
    rejects(model, "a tag without a name is rejected");
}

void source_references_are_complete() {
    Json model = base_model();
    model["tags"][0].erase("source");
    rejects(model, "a tag without a source reference is rejected");

    model = base_model();
    model["tags"][0]["source"] = "PumpA.Flow";
    rejects(model, "a source reference that is not an object is rejected");

    model = base_model();
    model["tags"][0]["source"].erase("identifier");
    rejects(model, "a source reference without an identifier is rejected");

    model = base_model();
    model["tags"][0]["source"]["server"] = "";
    rejects(model, "an empty source server is rejected");

    model = base_model();
    model["tags"][0]["source"].erase("namespace_uri");
    rejects(model, "a source reference without a namespace uri is rejected");
}

void alarm_declarations_are_bounded() {
    Json model = base_model();
    model["alarms"][0]["tag_id"] = "tank-a.unknown";
    rejects(model, "an alarm on an undeclared tag is rejected");

    model = base_model();
    model["alarms"][0]["asset_id"] = "tank-b";
    rejects(model, "an alarm whose owner does not own its tag is rejected");

    model = base_model();
    model["alarms"][0]["tag_id"] = "pump-a.state";
    model["alarms"][0]["asset_id"] = "pump-a";
    rejects(model, "an alarm bound to a non-numeric tag is rejected");

    model = base_model();
    model["alarms"][0]["operator"] = "=>";
    rejects(model, "an unsupported alarm operator is rejected");

    model = base_model();
    model["alarms"][0]["severity"] = "urgent";
    rejects(model, "an unsupported alarm severity is rejected");

    model = base_model();
    model["alarms"][0]["severity"] = "HIGH";
    expects_no_throw([&] { (void)validate_model(model); }, "alarm severity ignores case");

    model = base_model();
    model["alarms"][0]["threshold"] = "90";
    rejects(model, "a threshold declared as a string is rejected");

    model = base_model();
    model["alarms"][0]["threshold"] = std::numeric_limits<double>::infinity();
    rejects(model, "a non-finite threshold is rejected");

    model = base_model();
    model["alarms"][0]["asset_id"] = "asset-unknown";
    rejects(model, "an alarm owned by an undeclared asset is rejected");
}

/** An undeclared field is refused rather than ignored, at every level of the model. */
void undeclared_fields_are_refused() {
    Json model = base_model();
    model["command_endpoint"] = "opc.tcp://plant.invalid:4840";
    rejects(model, "an undeclared top level model field is refused");

    model = base_model();
    model["assets"][0]["write_address"] = "ns=2;s=Tank3.Setpoint";
    rejects(model, "an undeclared asset field is refused");

    model = base_model();
    model["tags"][0]["writable"] = true;
    rejects(model, "an undeclared tag field is refused");

    model = base_model();
    model["tags"][0]["source"]["endpoint"] = "opc.tcp://plant.invalid:4840";
    rejects(model, "an undeclared source reference field is refused");

    model = base_model();
    model["alarms"][0]["auto_acknowledge"] = true;
    rejects(model, "an undeclared alarm field is refused");

    model = base_model();
    model["relationships"][0]["weight"] = 3;
    rejects(model, "an undeclared relationship field is refused");
}

/** Declared metadata is bounded and structured, not a free text side channel. */
void declared_metadata_is_bounded() {
    Json model = base_model();
    model["metadata"] = Json{{"owner", "commissioning"}};
    expects_no_throw([&] { (void)validate_model(model); },
                     "a small structured model metadata object is accepted");

    model = base_model();
    model["metadata"] = "commissioning";
    rejects(model, "model metadata that is not an object is refused");

    model = base_model();
    model["metadata"] = Json{{"padding", std::string(17 * 1024, 'x')}};
    rejects(model, "model metadata above its declared size limit is refused");

    model = base_model();
    model["assets"][0]["metadata"] = Json{{"padding", std::string(5 * 1024, 'x')}};
    rejects(model, "asset metadata above its declared size limit is refused");
}

/** A control character in a declared string is refused, not carried into a view. */
void control_characters_are_refused() {
    const std::string injected = std::string("Station") + '\x01' + "A";

    Json model = base_model();
    model["name"] = injected;
    rejects(model, "a control character in the model name is refused");

    model = base_model();
    model["assets"][0]["name"] = injected;
    rejects(model, "a control character in an asset name is refused");

    model = base_model();
    model["assets"][0]["aliases"] = Json::array({injected});
    rejects(model, "a control character in an alias is refused");

    model = base_model();
    model["tags"][0]["unit"] = injected;
    rejects(model, "a control character in a unit is refused");

    model = base_model();
    model["tags"][0]["source"]["identifier"] = injected;
    rejects(model, "a control character in a source identifier is refused");
}

/** Identity fields are ASCII identifiers, so a lookalike character cannot become an ID. */
void confusable_identifiers_are_refused_outright() {
    const std::string confusable_asset = std::string("tank-") + kCyrillicSmallA;

    Json model = base_model();
    model["assets"].push_back(make_asset(confusable_asset, "Tank lookalike", "tank"));
    rejects(model, "an asset id containing a Cyrillic lookalike is refused");

    model = base_model();
    model["assets"][0]["id"] = confusable_asset;
    model["tags"][0]["asset_id"] = confusable_asset;
    model["alarms"][0]["asset_id"] = confusable_asset;
    model["relationships"][0]["to"] = confusable_asset;
    rejects(model, "a lookalike identity is refused even when every reference agrees");

    model = base_model();
    model["tags"][0]["id"] = std::string("tank-a.") + kCyrillicSmallA;
    model["alarms"][0]["tag_id"] = std::string("tank-a.") + kCyrillicSmallA;
    rejects(model, "a tag id containing a Cyrillic lookalike is refused");

    model = base_model();
    model["model_id"] = std::string("station-") + kCyrillicSmallA;
    rejects(model, "a model id containing a Cyrillic lookalike is refused");

    model = base_model();
    model["assets"][0]["id"] = "-tank-a";
    model["tags"][0]["asset_id"] = "-tank-a";
    model["alarms"][0]["asset_id"] = "-tank-a";
    model["relationships"][0]["to"] = "-tank-a";
    rejects(model, "an identifier that does not begin with an alphanumeric is refused");

    model = base_model();
    model["assets"][0]["id"] = "tank a";
    model["tags"][0]["asset_id"] = "tank a";
    model["alarms"][0]["asset_id"] = "tank a";
    model["relationships"][0]["to"] = "tank a";
    rejects(model, "an identifier containing a space is refused");
}

/**
 * Display names and aliases stay free text, so the lookalike risk moves there. A Latin
 * prompt must not reach an asset whose name only looks Latin.
 */
void confusable_display_names_do_not_merge_assets() {
    const std::string confusable_name = std::string("Tank ") + kCyrillicCapitalA;

    Json model = base_model();
    model["assets"].push_back(make_asset("tank-lookalike", confusable_name, "tank"));
    model["tags"].push_back(make_tag("tank-lookalike.level", "tank-lookalike", "Level", "level",
                                     "number", "%", "TankLookalike.Level"));

    Json summary;
    expects_no_throw([&] { summary = validate_model(model); },
                     "a lookalike display name is valid free text");
    check(summary.at("asset_count") == 5, "the lookalike named asset is a separate identity");

    const Json view = context_hmi::resolve_task(
        model, Json{{"kind", "overview"},
                    {"anchor_asset_id", "tank-lookalike"},
                    {"model_revision", 1},
                    {"original_request", "Show overview for the lookalike"}});
    check(view.at("components").size() == 1,
          "the lookalike named asset resolves only its own declared tag");
    check(view.at("components").at(0).at("tag_id") == "tank-lookalike.level",
          "the lookalike named asset never binds the Latin asset's tag");
}

}  /* namespace */

int main() {
    accepted_model_reports_declared_counts();
    model_envelope_fields_are_bounded();
    cardinality_limits_are_enforced();
    alias_limits_are_enforced();
    duplicate_identities_are_rejected();
    relationships_must_resolve_to_declared_assets();
    tag_declarations_are_bounded();
    source_references_are_complete();
    alarm_declarations_are_bounded();
    undeclared_fields_are_refused();
    declared_metadata_is_bounded();
    control_characters_are_refused();
    confusable_identifiers_are_refused_outright();
    confusable_display_names_do_not_merge_assets();
    return context_hmi_test::report("validation tests");
}
