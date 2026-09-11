#include "context_hmi/context_import_internal.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace context_hmi::context_import {
namespace {

constexpr std::string_view kManifestSchema = "engineering-bundle/1";
constexpr std::string_view kReportSchema = "engineering-import-report/1";

Result import_impl(const std::filesystem::path& manifest_path, const Options& options) {
  const auto manifest_contents = detail::read_bounded(manifest_path, options.limits);
  const auto manifest = detail::parse_json(manifest_contents, manifest_path.string(), options.limits);
  if (!manifest.is_object()) {
    detail::fail("invalid_manifest", manifest_path.string(), "manifest must be an object");
  }
  detail::allowed_keys(manifest, {"schema_version", "machine", "artifacts"}, manifest_path.string());
  if (manifest.value("schema_version", "") != kManifestSchema) {
    detail::fail("unsupported_manifest", manifest_path.string(), "manifest schema_version is unsupported");
  }
  const auto& machine = manifest.at("machine");
  if (!machine.is_object()) {
    detail::fail("invalid_manifest", manifest_path.string(), "machine must be an object");
  }
  detail::allowed_keys(machine, {"id", "name", "description", "revision"}, manifest_path.string());
  const auto machine_id = detail::string_field(machine, "id", "manifest.machine",
                                                options.limits.max_string_bytes);
  const auto machine_name = detail::string_field(machine, "name", "manifest.machine",
                                                  options.limits.max_string_bytes);
  if (machine.contains("description")) {
    detail::string_field(machine, "description", "manifest.machine", options.limits.max_string_bytes, true);
  }
  if (!detail::valid_identity(machine_id)) {
    detail::fail("invalid_identity", "manifest.machine.id", "machine id contains unsupported characters");
  }
  if (!machine.contains("revision") || !machine.at("revision").is_number_unsigned() ||
      machine.at("revision") == 0U ||
      machine.at("revision").get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
    detail::fail("invalid_revision", "manifest.machine.revision",
                 "revision must be a positive unsigned integer");
  }
  const auto revision = machine.at("revision").get<std::uint64_t>();
  if (!manifest.contains("artifacts") || !manifest.at("artifacts").is_array() ||
      manifest.at("artifacts").empty() || manifest.at("artifacts").size() > options.limits.max_artifacts) {
    detail::fail("invalid_manifest", manifest_path.string(), "artifacts must be a bounded non-empty array");
  }

  detail::ImportState state{
      Json{{"model_id", machine_id},
           {"revision", revision},
           {"name", machine_name},
           {"assets", Json::array()},
           {"relationships", Json::array()},
           {"tags", Json::array()},
           {"alarms", Json::array()}},
      Json{{"schema_version", kReportSchema},
           {"status", "ok"},
           {"machine", Json{{"id", machine_id}, {"name", machine_name}, {"revision", revision}}},
           {"source", Json{{"manifest", manifest_path.filename().generic_string()},
                            {"bundle_format", "engineering-bundle/1"},
                            {"read_only", true}}},
           {"artifacts", Json::array()},
           {"counts", Json{{"assets", 0}, {"relationships", 0}, {"tags", 0}, {"io", 0},
                            {"alarms", 0}, {"communications", 0}, {"documents", 0}}},
           {"unresolved", Json::array()},
           {"errors", Json::array()}},
      {},
      {},
      {},
      {},
      {},
      0,
      &options.limits};
  if (machine.contains("description") && !machine.at("description").get<std::string>().empty()) {
    state.model["description"] = machine.at("description");
  }

  std::vector<detail::Artifact> artifacts;
  artifacts.reserve(manifest.at("artifacts").size());
  std::unordered_set<std::string> artifact_ids;
  std::unordered_set<std::string> artifact_kinds;
  for (std::size_t index = 0; index < manifest.at("artifacts").size(); ++index) {
    const auto& entry = manifest.at("artifacts").at(index);
    const auto path = "manifest.artifacts[" + std::to_string(index) + "]";
    if (!entry.is_object()) {
      detail::fail("invalid_manifest", path, "artifact entry must be an object");
    }
    detail::allowed_keys(entry, {"id", "kind", "format", "path"}, path);
    detail::Artifact artifact{detail::string_field(entry, "id", path, options.limits.max_string_bytes),
                              detail::string_field(entry, "kind", path, options.limits.max_string_bytes),
                              detail::string_field(entry, "format", path, options.limits.max_string_bytes),
                              {},
                              {}};
    if (!detail::valid_identity(artifact.id) || !artifact_ids.emplace(artifact.id).second ||
        !artifact_kinds.emplace(artifact.kind).second) {
      detail::fail("invalid_identity", path + ".id", "artifact id is invalid or duplicated");
    }
    const auto expected_format = [&]() -> std::string_view {
      if (artifact.kind == "asset_hierarchy" || artifact.kind == "communications" ||
          artifact.kind == "machine_documents") {
        return "json";
      }
      if (artifact.kind == "relationships" || artifact.kind == "controller_tags" ||
          artifact.kind == "io_configuration" || artifact.kind == "alarm_definitions") {
        return "csv-rfc4180";
      }
      detail::fail("unsupported_artifact", path + ".kind", "artifact kind is not supported");
    }();
    if (artifact.format != expected_format) {
      detail::fail("unsupported_format", path + ".format", "artifact format does not match its kind");
    }
    const auto relative = detail::string_field(entry, "path", path, options.limits.max_string_bytes);
    artifact.path = detail::contained_path(manifest_path.parent_path(), relative, path + ".path");
    artifact.contents = detail::read_bounded(artifact.path, options.limits);
    artifacts.push_back(std::move(artifact));
  }

  auto find_artifact = [&](std::string_view kind) -> const detail::Artifact& {
    for (const auto& artifact : artifacts) {
      if (artifact.kind == kind) {
        return artifact;
      }
    }
    detail::fail("missing_artifact", "manifest.artifacts",
                 "required artifact kind is missing: " + std::string(kind));
  };
  auto optional_artifact = [&](std::string_view kind) -> const detail::Artifact* {
    for (const auto& artifact : artifacts) {
      if (artifact.kind == kind) {
        return &artifact;
      }
    }
    return nullptr;
  };

  detail::parse_assets(find_artifact("asset_hierarchy"), state);
  detail::parse_relationships(find_artifact("relationships"), state);
  detail::parse_tags(find_artifact("controller_tags"), state);
  if (const auto* artifact = optional_artifact("io_configuration"); artifact != nullptr) {
    detail::parse_io(*artifact, state);
  }
  detail::parse_alarms(find_artifact("alarm_definitions"), state);
  if (const auto* artifact = optional_artifact("communications"); artifact != nullptr) {
    detail::parse_metadata(*artifact, "communications", state,
                           {"id", "protocol", "endpoint", "security_mode"});
  }
  if (const auto* artifact = optional_artifact("machine_documents"); artifact != nullptr) {
    detail::parse_metadata(*artifact, "documents", state, {"id", "title", "path", "media_type"});
  }

  detail::sort_by_id(state.model.at("assets"));
  detail::sort_by_id(state.model.at("relationships"));
  detail::sort_by_id(state.model.at("tags"));
  detail::sort_by_id(state.model.at("alarms"));
  std::sort(state.report.at("artifacts").begin(), state.report.at("artifacts").end(),
            [](const Json& left, const Json& right) {
              return left.at("id").get<std::string>() < right.at("id").get<std::string>();
            });
  state.report["counts"]["assets"] = state.model.at("assets").size();
  state.report["counts"]["relationships"] = state.model.at("relationships").size();
  state.report["counts"]["tags"] = state.model.at("tags").size();
  state.report["counts"]["alarms"] = state.model.at("alarms").size();
  state.report["status"] = state.report.at("errors").empty()
                               ? (state.report.at("unresolved").empty() ? "ok" : "unresolved")
                               : "error";
  state.model["metadata"] = Json{{"provenance", Json{{"schema", "engineering-bundle/1"},
                                                       {"manifest", manifest_path.filename().generic_string()},
                                                       {"artifacts", state.report.at("artifacts")}}}};
  return Result{std::move(state.model), std::move(state.report)};
}

}  /* namespace */

ImportError::ImportError(std::string error_code, std::string error_path, std::string message)
    : std::runtime_error(std::move(message)), code(std::move(error_code)), path(std::move(error_path)) {}

Result import_bundle(const std::filesystem::path& bundle, const Options& options) {
  std::error_code error;
  if (!std::filesystem::is_directory(bundle, error) || error) {
    throw ImportError("bundle_unavailable", bundle.string(), "bundle is not a directory");
  }
  return import_manifest(bundle / "manifest.json", options);
}

Result import_manifest(const std::filesystem::path& manifest, const Options& options) {
  return import_impl(manifest, options);
}

}  /* namespace context_hmi::context_import */
