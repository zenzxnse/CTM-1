/*
 * Vendor-neutral engineering bundle import tests.
 *
 * The importer is a trust boundary. These tests verify deterministic native output, strict
 * source provenance, explicit ownership and relationship declarations, and bounded rejection
 * of malformed or adversarial bundles.
 */

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "context_hmi/context_import.hpp"
#include "context_hmi/engine.hpp"
#include "support/check.hpp"

using context_hmi::Json;
using context_hmi::context_import::ImportError;
using context_hmi::context_import::Limits;
using context_hmi::context_import::Options;
using context_hmi::context_import::Result;
using context_hmi_test::check;
using context_hmi_test::expects_no_throw;

namespace {

const std::filesystem::path kFixture = "examples/artifacts";

class TemporaryBundle final {
 public:
  TemporaryBundle() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path();
    for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
      path_ = base / ("context-hmi-import-" + std::to_string(stamp) + "-" +
                      std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error) && !error) {
        for (const auto &entry : std::filesystem::directory_iterator(kFixture)) {
          std::filesystem::copy(entry.path(), path_ / entry.path().filename(),
                                std::filesystem::copy_options::recursive, error);
          if (error) {
            break;
          }
        }
        if (!error) {
          return;
        }
        std::filesystem::remove_all(path_);
      }
    }
    throw std::runtime_error("cannot create an isolated importer fixture");
  }

  ~TemporaryBundle() { std::error_code error; std::filesystem::remove_all(path_, error); }

  TemporaryBundle(const TemporaryBundle &) = delete;
  TemporaryBundle &operator=(const TemporaryBundle &) = delete;

  const std::filesystem::path &path() const { return path_; }

 private:
  std::filesystem::path path_;
};

Json read_json(const std::filesystem::path &path) {
  std::ifstream input(path);
  return Json::parse(input);
}

void write_json(const std::filesystem::path &path, const Json &value) {
  std::ofstream output(path, std::ios::trunc);
  output << value.dump(2) << '\n';
}

void append_text(const std::filesystem::path &path, const std::string &text) {
  std::ofstream output(path, std::ios::app);
  output << text;
}

template <typename Function>
void expects_import_error(Function &&function, const std::string &code,
                          const std::string &message) {
  try {
    function();
    check(false, message + " (no ImportError)");
  } catch (const ImportError &error) {
    check(error.code == code,
          message + " (expected code '" + code + "', got '" + error.code + "')");
  } catch (const std::exception &error) {
    check(false, message + " (wrong exception: " + std::string(error.what()) + ")");
  }
}

void successful_import_is_deterministic_and_engine_valid() {
  const Result first = context_hmi::context_import::import_bundle(kFixture);
  const Result second = context_hmi::context_import::import_bundle(kFixture);
  check(first.model == second.model, "repeated imports produce identical model JSON");
  check(first.report == second.report, "repeated imports produce identical report JSON");
  check(first.report.at("schema_version") == "engineering-import-report/1",
        "the report has the versioned import schema");
  check(first.report.at("status") == "ok", "the complete fixture imports without unresolved data");
  expects_no_throw([&] { (void)context_hmi::validate_model(first.model); },
                   "imported model satisfies the native engine schema");
  check(first.model.at("model_id") == "assembly-line-a", "machine identity is retained");
  check(first.model.at("revision") == 1, "machine revision is retained");
  check(first.model.at("assets").size() == 4, "four explicit assets are imported");
  check(first.model.at("relationships").size() == 5, "five explicit relationships are imported");
  check(first.model.at("tags").size() == 8, "eight explicitly owned tags are imported");
  check(first.model.at("alarms").size() == 3, "three alarms with explicit references are imported");
}

void every_artifact_class_and_provenance_is_reported() {
  const Result result = context_hmi::context_import::import_bundle(kFixture);
  const std::map<std::string, std::tuple<std::string, std::string, std::size_t>> expected{
      {"alarms", {"alarm_definitions", "csv-rfc4180", 3U}},
      {"assets", {"asset_hierarchy", "json", 4U}},
      {"communications", {"communications", "json", 1U}},
      {"documents", {"machine_documents", "json", 2U}},
      {"io", {"io_configuration", "csv-rfc4180", 8U}},
      {"links", {"relationships", "csv-rfc4180", 5U}},
      {"tags", {"controller_tags", "csv-rfc4180", 8U}}};
  check(result.report.at("artifacts").size() == expected.size(),
        "all seven artifact classes have provenance records");
  for (const auto &entry : result.report.at("artifacts")) {
    const auto id = entry.at("id").get<std::string>();
    check(expected.contains(id), "artifact provenance uses a declared artifact id");
    if (!expected.contains(id)) {
      continue;
    }
    const auto &spec = expected.at(id);
    check(entry.at("kind") == std::get<0>(spec), "artifact kind is retained");
    check(entry.at("format") == std::get<1>(spec), "artifact format is retained");
    check(entry.at("records") == std::get<2>(spec), "artifact record count is retained");
    check(entry.at("bytes") == std::filesystem::file_size(kFixture / entry.at("path").get<std::string>()),
          "artifact byte size matches the source file");
    check(entry.at("path").is_string() && entry.at("path").get<std::string>().find('/') ==
                                              std::string::npos,
          "provenance path is the bundle-local filename, not an absolute local path");
    check(entry.at("fingerprint").get<std::string>().rfind("fnv1a64:", 0) == 0,
          "artifact content fingerprint is recorded");
  }
  check(result.report.at("counts") ==
            Json{{"assets", 4}, {"relationships", 5}, {"tags", 8}, {"io", 8},
                 {"alarms", 3}, {"communications", 1}, {"documents", 2}},
        "report counts cover every artifact class");
  check(result.model.at("metadata").at("provenance").at("schema") == "engineering-bundle/1",
        "native model carries the import schema provenance");
  check(result.model.at("metadata").at("provenance").at("artifacts") ==
            result.report.at("artifacts"),
        "native model provenance exactly matches the import report");
}

void metadata_artifacts_never_become_native_control_data() {
  const Result result = context_hmi::context_import::import_bundle(kFixture);
  check(!result.model.contains("io"), "I/O metadata is not copied into the native model");
  check(!result.model.contains("communications"), "communications metadata is not copied into the model");
  check(!result.model.contains("documents"), "document references are not copied into the native model");
  check(result.model.dump().find("PLC1:") == std::string::npos,
        "controller addresses do not cross into native model output");
  check(result.model.dump().find("opc.tcp://") == std::string::npos,
        "communication endpoints do not cross into native model output");
  for (const auto &tag : result.model.at("tags")) {
    check(tag.contains("asset_id") && tag.at("asset_id").is_string(),
          "each imported tag retains its explicit ownership field");
    check(tag.at("source").contains("identifier"), "each imported tag retains source identity");
  }
}

void unresolved_references_are_visible_and_never_inferred() {
  TemporaryBundle bundle;
  append_text(bundle.path() / "relationships.csv",
              "display-name-link,Assembly Line A,oven-a1,contains\n");
  append_text(bundle.path() / "tags.csv",
              "display-reading,Assembly Line A,Display reading,temperature,number,degC,plant-broker,urn:factory:assembly,LineA.Display\n");
  append_text(bundle.path() / "io.csv", "io-display,display-reading,AI,PLC1:AI.99,99\n");
  append_text(bundle.path() / "alarms.csv",
              "display-alarm,Assembly Line A,Display alarm,display-reading,>=,1,low\n");
  const Result result = context_hmi::context_import::import_bundle(bundle.path());
  check(result.report.at("status") == "unresolved", "unresolved references produce an explicit status");
  check(result.model.at("relationships").size() == 5,
        "a relationship using a display name is not inferred");
  check(result.model.at("tags").size() == 8,
        "a tag using a display name as owner is not inferred");
  check(result.model.at("alarms").size() == 3,
        "an alarm using unresolved owner and tag is not inferred");
  std::set<std::string> codes;
  for (const auto &item : result.report.at("unresolved")) {
    codes.insert(item.at("code").get<std::string>());
  }
  check(codes.contains("unknown_relationship_endpoint"), "unknown relationship endpoint is reported");
  check(codes.contains("unknown_tag_owner"), "unknown tag ownership is reported");
  check(codes.contains("unknown_io_tag"), "unknown I/O tag reference is reported");
  check(codes.contains("unknown_alarm_reference"), "unknown alarm reference is reported");
}

void malformed_manifest_and_artifacts_are_rejected() {
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["schema_version"] = "engineering-bundle/999";
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "unsupported_manifest", "unsupported manifest schema is rejected");
  }
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["unexpected"] = true;
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "unknown_field", "unknown manifest fields are rejected");
  }
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["artifacts"][0]["format"] = "csv-rfc4180";
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "unsupported_format", "artifact format mismatches are rejected");
  }
  {
    TemporaryBundle bundle;
    auto assets = read_json(bundle.path() / "assets.json");
    assets.at(0).erase("id");
    write_json(bundle.path() / "assets.json", assets);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_field", "malformed artifact records are rejected");
  }
  {
    TemporaryBundle bundle;
    std::ofstream output(bundle.path() / "assets.json", std::ios::trunc);
    output << "[{\"id\": ";
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_json", "malformed JSON artifacts are rejected");
  }
  {
    TemporaryBundle bundle;
    append_text(bundle.path() / "tags.csv", "broken,\"unterminated\n");
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_csv", "malformed CSV quoting is rejected");
  }
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["artifacts"][0]["path"] = "../outside.json";
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_artifact_path", "directory traversal artifact paths are rejected");
  }
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["artifacts"][0]["path"] = std::filesystem::absolute("outside.json").generic_string();
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_artifact_path", "absolute artifact paths are rejected");
  }
}

void duplicate_identities_and_missing_declarations_are_rejected() {
  {
    TemporaryBundle bundle;
    auto assets = read_json(bundle.path() / "assets.json");
    assets.push_back(assets.at(0));
    write_json(bundle.path() / "assets.json", assets);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_identity", "duplicate asset identities are rejected");
  }
  {
    TemporaryBundle bundle;
    append_text(bundle.path() / "relationships.csv",
                "line-has-conveyor,line-a,conveyor-a1,contains\n");
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_identity", "duplicate relationship identities are rejected");
  }
  {
    TemporaryBundle bundle;
    append_text(bundle.path() / "tags.csv",
                "line-a.state,line-a,Duplicate state,operating_state,boolean,,plant-broker,urn:factory:assembly,LineA.Running2\n");
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_identity", "duplicate tag identities are rejected");
  }
  {
    TemporaryBundle bundle;
    append_text(bundle.path() / "alarms.csv",
                "oven-a1.high-temperature,oven-a1,Duplicate alarm,oven-a1.temperature,>=,205,high\n");
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "invalid_identity", "duplicate alarm identities are rejected");
  }
  {
    TemporaryBundle bundle;
    auto manifest = read_json(bundle.path() / "manifest.json");
    manifest["artifacts"].erase(manifest["artifacts"].begin() + 4);
    write_json(bundle.path() / "manifest.json", manifest);
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(bundle.path()); },
                         "missing_artifact", "missing required artifact kinds are rejected");
  }
}

void configured_limits_are_hard_bounds() {
  {
    Options options;
    options.limits.max_artifacts = 6;
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "invalid_manifest", "artifact count limit is enforced");
  }
  {
    Options options;
    options.limits.max_records_per_artifact = 3;
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "too_many_records", "per-artifact record limit is enforced");
  }
  {
    Options options;
    options.limits.max_total_records = 3;
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "too_many_records", "total record limit is enforced");
  }
  {
    Options options;
    options.limits.max_columns = 8;
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "too_many_columns", "CSV column limit is enforced");
  }
  {
    Options options;
    options.limits.max_file_bytes = std::filesystem::file_size(kFixture / "manifest.json");
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "artifact_too_large", "artifact byte limit is enforced");
  }
  {
    Options options;
    options.limits.max_json_depth = 0;
    expects_import_error([&] { (void)context_hmi::context_import::import_bundle(kFixture, options); },
                         "json_too_complex", "JSON depth limit is enforced");
  }
}

}  /* namespace */

int main() {
  successful_import_is_deterministic_and_engine_valid();
  every_artifact_class_and_provenance_is_reported();
  metadata_artifacts_never_become_native_control_data();
  unresolved_references_are_visible_and_never_inferred();
  malformed_manifest_and_artifacts_are_rejected();
  duplicate_identities_and_missing_declarations_are_rejected();
  configured_limits_are_hard_bounds();
  return context_hmi_test::report("context import tests");
}
