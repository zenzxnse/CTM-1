#include "context_hmi/context_import.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace context_hmi::context_import {
namespace {

constexpr std::string_view kManifestSchema = "engineering-bundle/1";
constexpr std::string_view kReportSchema = "engineering-import-report/1";

using Rows = std::vector<std::vector<std::string>>;

[[noreturn]] void fail(std::string code, const std::string& path, std::string message) {
  throw ImportError(std::move(code), path, std::move(message));
}

void require_string(const Json& object, std::string_view key, const std::string& path,
                   std::size_t limit, bool allow_empty = false) {
  if (!object.contains(key) || !object.at(key).is_string()) {
    fail("invalid_field", path, "required string field is missing");
  }
  const auto& value = object.at(key).get_ref<const std::string&>();
  if ((!allow_empty && value.empty()) || value.size() > std::min(limit, std::size_t{256}) ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) {
    fail("invalid_field", path, "string field is empty or exceeds its bound");
  }
}

std::string string_field(const Json& object, std::string_view key, const std::string& path,
                         std::size_t limit, bool allow_empty = false) {
  require_string(object, key, path, limit, allow_empty);
  return object.at(key).get<std::string>();
}

void allowed_keys(const Json& object, std::initializer_list<std::string_view> allowed,
                  const std::string& path) {
  std::set<std::string_view> keys(allowed.begin(), allowed.end());
  for (const auto& item : object.items()) {
    const auto& key = item.key();
    if (!keys.contains(key)) {
      fail("unknown_field", path, "field is not accepted: " + key);
    }
  }
}

std::string trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
          value[first] == '\n')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' ||
          value[last - 1] == '\n')) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

std::string read_bounded(const std::filesystem::path& path, const Limits& limits) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    fail("artifact_unavailable", path.string(), "artifact is not a regular file");
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > limits.max_file_bytes) {
    fail("artifact_too_large", path.string(), "artifact exceeds the configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("artifact_unavailable", path.string(), "artifact cannot be opened");
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input) {
      fail("artifact_unavailable", path.string(), "artifact could not be read completely");
    }
  }
  return contents;
}

void validate_json_limits(const Json& value, const std::string& path, const Limits& limits) {
  std::vector<std::pair<const Json*, std::size_t>> pending{{&value, 0U}};
  std::size_t nodes = 0;
  while (!pending.empty()) {
    const auto [current, depth] = pending.back();
    pending.pop_back();
    if (++nodes > limits.max_json_nodes || depth > limits.max_json_depth) {
      fail("json_too_complex", path, "JSON artifact exceeds the configured depth or node limit");
    }
    if (current->is_string() && current->get_ref<const std::string&>().size() > limits.max_string_bytes) {
      fail("field_too_large", path, "JSON string exceeds the configured byte limit");
    }
    if (current->is_object()) {
      for (const auto& item : current->items()) {
        if (item.key().size() > limits.max_string_bytes) {
          fail("field_too_large", path, "JSON key exceeds the configured byte limit");
        }
        pending.emplace_back(&item.value(), depth + 1U);
      }
    } else if (current->is_array()) {
      for (const auto& item : *current) {
        pending.emplace_back(&item, depth + 1U);
      }
    }
  }
}

Json parse_json(const std::string& contents, const std::string& path, const Limits& limits) {
  try {
    auto value = Json::parse(contents);
    validate_json_limits(value, path, limits);
    return value;
  } catch (const Json::parse_error& error) {
    fail("invalid_json", path, error.what());
  }
}

std::string fingerprint(std::string_view contents) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char character : contents) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return output.str();
}

Rows parse_csv(std::string_view contents, const std::string& path, const Limits& limits) {
  Rows rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  bool after_quote = false;
  bool saw_character = false;
  auto check_field = [&]() {
    if (field.size() > limits.max_field_bytes) {
      fail("field_too_large", path, "CSV field exceeds the configured byte limit");
    }
  };
  auto finish_row = [&]() {
    check_field();
    row.push_back(std::move(field));
    field.clear();
    if (row.size() > limits.max_columns) {
      fail("too_many_columns", path, "CSV row exceeds the configured column limit");
    }
    if (row.size() == 1U && row.front().empty() && !saw_character) {
      row.clear();
      return;
    }
    rows.push_back(std::move(row));
    row.clear();
    saw_character = false;
    if (rows.size() > limits.max_records_per_artifact + 1U) {
      fail("too_many_records", path, "CSV artifact exceeds the configured record limit");
    }
  };

  for (std::size_t index = 0; index < contents.size(); ++index) {
    const char character = contents[index];
    if (quoted) {
      if (character == '"') {
        if (index + 1U < contents.size() && contents[index + 1U] == '"') {
          field.push_back('"');
          ++index;
        } else {
          quoted = false;
          after_quote = true;
        }
      } else {
        field.push_back(character);
        saw_character = true;
        check_field();
      }
      continue;
    }
    if (after_quote) {
      if (character == ',') {
        check_field();
        row.push_back(std::move(field));
        field.clear();
        if (row.size() >= limits.max_columns) {
          fail("too_many_columns", path, "CSV row exceeds the configured column limit");
        }
        after_quote = false;
      } else if (character == '\r' || character == '\n') {
        after_quote = false;
        if (character == '\r' && index + 1U < contents.size() && contents[index + 1U] == '\n') {
          ++index;
        }
        finish_row();
      } else if (character == ' ' || character == '\t') {
        fail("invalid_csv", path, "whitespace after a quoted field is not accepted");
      } else {
        fail("invalid_csv", path, "characters after a quoted field are not accepted");
      }
      continue;
    }
    if (character == '"') {
      if (!field.empty()) {
        fail("invalid_csv", path, "a quote may start only an empty field");
      }
      quoted = true;
      saw_character = true;
    } else if (character == ',') {
      check_field();
      row.push_back(std::move(field));
      field.clear();
      if (row.size() >= limits.max_columns) {
        fail("too_many_columns", path, "CSV row exceeds the configured column limit");
      }
    } else if (character == '\r' || character == '\n') {
      if (character == '\r' && index + 1U < contents.size() && contents[index + 1U] == '\n') {
        ++index;
      }
      finish_row();
    } else {
      field.push_back(character);
      saw_character = true;
      check_field();
    }
  }
  if (quoted) {
    fail("invalid_csv", path, "quoted field is not closed");
  }
  if (after_quote || !field.empty() || !row.empty()) {
    finish_row();
  }
  if (rows.empty()) {
    fail("invalid_csv", path, "CSV artifact has no header row");
  }
  return rows;
}

std::map<std::string, std::size_t> header_map(const Rows& rows, const std::string& path,
                                              const Limits& limits) {
  std::map<std::string, std::size_t> headers;
  for (std::size_t index = 0; index < rows.front().size(); ++index) {
    const auto header = trim(rows.front()[index]);
    if (header.empty() || header.size() > limits.max_string_bytes ||
        !headers.emplace(header, index).second) {
      fail("invalid_csv", path, "CSV header is empty, oversized or duplicated");
    }
  }
  return headers;
}

std::string cell(const std::vector<std::string>& row, const std::map<std::string, std::size_t>& headers,
                 std::string_view key, const std::string& path, const Limits& limits,
                 bool required = true) {
  const auto found = headers.find(std::string(key));
  if (found == headers.end()) {
    if (required) {
      fail("invalid_csv", path, "required column is missing: " + std::string(key));
    }
    return {};
  }
  if (found->second >= row.size()) {
    if (required) {
      fail("invalid_csv", path, "row has fewer cells than its header");
    }
    return {};
  }
  if (row[found->second].size() > std::min(limits.max_field_bytes, std::size_t{256})) {
    fail("field_too_large", path, "CSV field exceeds the configured byte limit");
  }
  if (required && row[found->second].empty()) {
    fail("invalid_field", path, "required CSV field is empty");
  }
  if (std::any_of(row[found->second].begin(), row[found->second].end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) {
    fail("invalid_field", path, "CSV field contains a control character");
  }
  return row[found->second];
}

void require_columns(const std::map<std::string, std::size_t>& headers,
                     std::initializer_list<std::string_view> required, const std::string& path) {
  for (const auto name : required) {
    if (!headers.contains(std::string(name))) {
      fail("invalid_csv", path, "required column is missing: " + std::string(name));
    }
  }
}

void check_row_width(const Rows& rows, const std::string& path, const Limits& limits) {
  for (std::size_t row = 1U; row < rows.size(); ++row) {
    if (rows[row].size() != rows.front().size()) {
      fail("invalid_csv", path, "row has a different number of cells than its header");
    }
  }
  if (rows.size() - 1U > limits.max_records_per_artifact) {
    fail("too_many_records", path, "CSV artifact exceeds the configured record limit");
  }
}

bool valid_identity(std::string_view value) {
  if (value.empty() || value.size() > 256U ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z') ||
        (value.front() >= '0' && value.front() <= '9'))) {
    return false;
  }
  for (const unsigned char character : value) {
    if (!(character == '_' || character == '-' || character == '.' ||
          (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9'))) {
      return false;
    }
  }
  return true;
}

Json records_array(const Json& value, const std::string& path, const Limits& limits) {
  if (value.is_array()) {
    if (value.size() > limits.max_records_per_artifact) {
      fail("too_many_records", path, "JSON artifact exceeds the configured record limit");
    }
    return value;
  }
  if (value.is_object() && value.contains("records") && value.at("records").is_array()) {
    if (value.at("records").size() > limits.max_records_per_artifact) {
      fail("too_many_records", path, "JSON artifact exceeds the configured record limit");
    }
    return value.at("records");
  }
  fail("invalid_json_shape", path, "artifact must be an array or an object with records");
}

void add_unresolved(Json& report, std::string code, std::string path, std::string message) {
  report.at("unresolved").push_back(Json{{"code", std::move(code)},
                                           {"path", std::move(path)},
                                           {"message", std::move(message)},
                                           {"severity", "warning"}});
}

struct Artifact {
  std::string id;
  std::string kind;
  std::string format;
  std::filesystem::path path;
  std::string contents;
};

std::filesystem::path contained_path(const std::filesystem::path& root,
                                     const std::string& relative, const std::string& path) {
  const std::filesystem::path candidate(relative);
  if (candidate.empty() || candidate.is_absolute()) {
    fail("invalid_artifact_path", path, "artifact path must be a non-empty relative path");
  }
  std::error_code error;
  const auto root_real = std::filesystem::weakly_canonical(root, error);
  if (error) {
    fail("bundle_unavailable", root.string(), "bundle path cannot be resolved");
  }
  const auto file_real = std::filesystem::weakly_canonical(root / candidate, error);
  if (error) {
    fail("artifact_unavailable", path, "artifact path cannot be resolved");
  }
  const auto relative_real = file_real.lexically_relative(root_real);
  if (relative_real.empty() || relative_real == ".." || relative_real.begin()->string() == "..") {
    fail("invalid_artifact_path", path, "artifact path escapes the bundle directory");
  }
  return file_real;
}

Json source_record(const Artifact& artifact, std::size_t records) {
  return Json{{"id", artifact.id},
              {"kind", artifact.kind},
              {"format", artifact.format},
              {"path", artifact.path.filename().generic_string()},
              {"bytes", artifact.contents.size()},
              {"records", records},
              {"fingerprint", fingerprint(artifact.contents)}};
}

void sort_by_id(Json& array) {
  std::sort(array.begin(), array.end(), [](const Json& left, const Json& right) {
    return left.at("id").get<std::string>() < right.at("id").get<std::string>();
  });
}

Json import_impl(const std::filesystem::path& manifest_path, const Options& options) {
  const auto manifest_contents = read_bounded(manifest_path, options.limits);
  const auto manifest = parse_json(manifest_contents, manifest_path.string(), options.limits);
  if (!manifest.is_object()) {
    fail("invalid_manifest", manifest_path.string(), "manifest must be an object");
  }
  allowed_keys(manifest, {"schema_version", "machine", "artifacts"}, manifest_path.string());
  if (manifest.value("schema_version", "") != kManifestSchema) {
    fail("unsupported_manifest", manifest_path.string(), "manifest schema_version is unsupported");
  }
  const auto& machine = manifest.at("machine");
  if (!machine.is_object()) {
    fail("invalid_manifest", manifest_path.string(), "machine must be an object");
  }
  allowed_keys(machine, {"id", "name", "description", "revision"}, manifest_path.string());
  const auto machine_id = string_field(machine, "id", "manifest.machine", options.limits.max_string_bytes);
  const auto machine_name = string_field(machine, "name", "manifest.machine", options.limits.max_string_bytes);
  if (machine.contains("description")) {
    string_field(machine, "description", "manifest.machine", options.limits.max_string_bytes, true);
  }
  if (!valid_identity(machine_id)) {
    fail("invalid_identity", "manifest.machine.id", "machine id contains unsupported characters");
  }
  if (!machine.contains("revision") || !machine.at("revision").is_number_unsigned() ||
      machine.at("revision") == 0U ||
      machine.at("revision").get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
    fail("invalid_revision", "manifest.machine.revision", "revision must be a positive unsigned integer");
  }
  const auto revision = machine.at("revision").get<std::uint64_t>();
  if (!manifest.contains("artifacts") || !manifest.at("artifacts").is_array() ||
      manifest.at("artifacts").empty() || manifest.at("artifacts").size() > options.limits.max_artifacts) {
    fail("invalid_manifest", manifest_path.string(), "artifacts must be a bounded non-empty array");
  }

  Json report{{"schema_version", kReportSchema},
              {"status", "ok"},
              {"machine", Json{{"id", machine_id}, {"name", machine_name}, {"revision", revision}}},
              {"source", Json{{"manifest", manifest_path.filename().generic_string()},
                               {"bundle_format", "engineering-bundle/1"},
                               {"read_only", true}}},
              {"artifacts", Json::array()},
              {"counts", Json{{"assets", 0}, {"relationships", 0}, {"tags", 0}, {"io", 0},
                               {"alarms", 0}, {"communications", 0}, {"documents", 0}}},
              {"unresolved", Json::array()},
              {"errors", Json::array()}};
  std::vector<Artifact> artifacts;
  std::unordered_set<std::string> artifact_ids;
  std::unordered_set<std::string> artifact_kinds;
  std::size_t total_records = 0;
  for (std::size_t index = 0; index < manifest.at("artifacts").size(); ++index) {
    const auto& entry = manifest.at("artifacts").at(index);
    const auto path = "manifest.artifacts[" + std::to_string(index) + "]";
    if (!entry.is_object()) {
      fail("invalid_manifest", path, "artifact entry must be an object");
    }
    allowed_keys(entry, {"id", "kind", "format", "path"}, path);
    Artifact artifact{string_field(entry, "id", path, options.limits.max_string_bytes),
                      string_field(entry, "kind", path, options.limits.max_string_bytes),
                      string_field(entry, "format", path, options.limits.max_string_bytes),
                      {},
                      {}};
    if (!valid_identity(artifact.id) || !artifact_ids.emplace(artifact.id).second ||
        !artifact_kinds.emplace(artifact.kind).second) {
      fail("invalid_identity", path + ".id", "artifact id is invalid or duplicated");
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
      fail("unsupported_artifact", path + ".kind", "artifact kind is not supported");
    }();
    if (artifact.format != expected_format) {
      fail("unsupported_format", path + ".format", "artifact format does not match its kind");
    }
    const auto relative = string_field(entry, "path", path, options.limits.max_string_bytes);
    artifact.path = contained_path(manifest_path.parent_path(), relative, path + ".path");
    artifact.contents = read_bounded(artifact.path, options.limits);
    artifacts.push_back(std::move(artifact));
  }

  auto find_artifact = [&](std::string_view kind) -> const Artifact& {
    for (const auto& artifact : artifacts) {
      if (artifact.kind == kind) {
        return artifact;
      }
    }
    fail("missing_artifact", "manifest.artifacts", "required artifact kind is missing: " + std::string(kind));
  };
  auto optional_artifact = [&](std::string_view kind) -> const Artifact* {
    for (const auto& artifact : artifacts) {
      if (artifact.kind == kind) {
        return &artifact;
      }
    }
    return nullptr;
  };
  Json model{{"model_id", machine_id},
             {"revision", revision},
             {"name", machine_name},
             {"assets", Json::array()},
             {"relationships", Json::array()},
             {"tags", Json::array()},
             {"alarms", Json::array()}};
  if (machine.contains("description") && !machine.at("description").get<std::string>().empty()) {
    model["description"] = machine.at("description");
  }
  std::unordered_set<std::string> assets;
  std::unordered_set<std::string> relationships;
  std::unordered_set<std::string> tags;
  std::unordered_set<std::string> emitted_tags;
  std::unordered_set<std::string> alarms;
  auto record_source = [&](const Artifact& artifact, std::size_t records) {
    total_records += records;
    if (total_records > options.limits.max_total_records) {
      fail("too_many_records", "manifest.artifacts", "bundle exceeds total record limit");
    }
    report.at("artifacts").push_back(source_record(artifact, records));
  };

  const auto& assets_artifact = find_artifact("asset_hierarchy");
  const auto assets_json = records_array(parse_json(assets_artifact.contents, assets_artifact.path.string(), options.limits),
                                         assets_artifact.path.string(), options.limits);
  for (std::size_t index = 0; index < assets_json.size(); ++index) {
    const auto path = assets_artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto& item = assets_json.at(index);
    if (!item.is_object()) {
      fail("invalid_record", path, "asset record must be an object");
    }
    allowed_keys(item, {"id", "name", "kind", "aliases", "description"}, path);
    const auto id = string_field(item, "id", path, options.limits.max_string_bytes);
    if (!valid_identity(id) || !assets.emplace(id).second) {
      fail("invalid_identity", path + ".id", "asset id is invalid or duplicated");
    }
    const auto name = string_field(item, "name", path, options.limits.max_string_bytes);
    const auto kind = string_field(item, "kind", path, options.limits.max_string_bytes);
    Json asset{{"id", id}, {"name", name}, {"kind", kind}};
    if (item.contains("aliases")) {
      if (!item.at("aliases").is_array() || item.at("aliases").size() > 32U) {
        fail("invalid_field", path + ".aliases", "aliases must be a bounded array");
      }
      asset["aliases"] = Json::array();
      for (const auto& alias : item.at("aliases")) {
        if (!alias.is_string() || alias.get<std::string>().empty() ||
            alias.get<std::string>().size() > std::min(options.limits.max_string_bytes, std::size_t{256}) ||
            std::any_of(alias.get_ref<const std::string&>().begin(), alias.get_ref<const std::string&>().end(),
                        [](unsigned char character) {
                          return character < 0x20U || character == 0x7FU;
                        })) {
          fail("invalid_field", path + ".aliases", "alias is invalid or oversized");
        }
        asset["aliases"].push_back(alias);
      }
    }
    if (item.contains("description")) {
      asset["metadata"] = Json{{"description", string_field(item, "description", path,
                                                             options.limits.max_string_bytes, true)}};
    }
    model.at("assets").push_back(std::move(asset));
  }
  record_source(assets_artifact, assets_json.size());

  const auto& relationships_artifact = find_artifact("relationships");
  const auto relationships_contents = relationships_artifact.contents;
  const auto relationship_rows = parse_csv(relationships_contents, relationships_artifact.path.string(), options.limits);
  const auto relationship_headers = header_map(relationship_rows, relationships_artifact.path.string(), options.limits);
  require_columns(relationship_headers, {"id", "from", "to", "kind"}, relationships_artifact.path.string());
  check_row_width(relationship_rows, relationships_artifact.path.string(), options.limits);
  for (std::size_t index = 1U; index < relationship_rows.size(); ++index) {
    const auto path = relationships_artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(relationship_rows[index], relationship_headers, "id", path, options.limits);
    const auto from = cell(relationship_rows[index], relationship_headers, "from", path, options.limits);
    const auto to = cell(relationship_rows[index], relationship_headers, "to", path, options.limits);
    const auto kind = cell(relationship_rows[index], relationship_headers, "kind", path, options.limits);
    if (!valid_identity(id) || !relationships.emplace(id).second) {
      fail("invalid_identity", path + ".id", "relationship id is invalid or duplicated");
    }
    if (from == to || !assets.contains(from) || !assets.contains(to)) {
      add_unresolved(report, "unknown_relationship_endpoint", path,
                     "relationship is not emitted because its endpoints are undeclared or identical");
      continue;
    }
    model.at("relationships").push_back(Json{{"id", id}, {"from", from}, {"to", to}, {"kind", kind}});
  }
  record_source(relationships_artifact, relationship_rows.size() - 1U);

  const auto& tags_artifact = find_artifact("controller_tags");
  const auto tag_rows = parse_csv(tags_artifact.contents, tags_artifact.path.string(), options.limits);
  const auto tag_headers = header_map(tag_rows, tags_artifact.path.string(), options.limits);
  require_columns(tag_headers, {"id", "asset_id", "name", "role", "data_type", "unit", "server",
                                "namespace_uri", "identifier"},
                  tags_artifact.path.string());
  check_row_width(tag_rows, tags_artifact.path.string(), options.limits);
  for (std::size_t index = 1U; index < tag_rows.size(); ++index) {
    const auto path = tags_artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(tag_rows[index], tag_headers, "id", path, options.limits);
    const auto owner = cell(tag_rows[index], tag_headers, "asset_id", path, options.limits);
    if (!valid_identity(id) || !tags.emplace(id).second) {
      fail("invalid_identity", path + ".id", "tag id is invalid or duplicated");
    }
    auto item = Json{{"id", id},
                           {"asset_id", owner},
                           {"name", cell(tag_rows[index], tag_headers, "name", path, options.limits)},
                           {"role", cell(tag_rows[index], tag_headers, "role", path, options.limits)},
                           {"data_type", cell(tag_rows[index], tag_headers, "data_type", path, options.limits)},
                           {"unit", cell(tag_rows[index], tag_headers, "unit", path, options.limits, false)},
                           {"source", Json{{"server", cell(tag_rows[index], tag_headers, "server", path, options.limits)},
                                            {"namespace_uri", cell(tag_rows[index], tag_headers, "namespace_uri", path, options.limits)},
                                            {"identifier", cell(tag_rows[index], tag_headers, "identifier", path, options.limits)}}}};
    auto data_type = item.at("data_type").get<std::string>();
    std::transform(data_type.begin(), data_type.end(), data_type.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    item["data_type"] = data_type;
    static const std::set<std::string> supported_data_types{
        "number", "float", "double", "integer", "boolean", "bool", "string"};
    if (!supported_data_types.contains(data_type)) {
      add_unresolved(report, "unsupported_tag_type", path,
                     "tag is not emitted because its data_type is not supported by the native model");
      continue;
    }
    if (!assets.contains(owner)) {
      add_unresolved(report, "unknown_tag_owner", path,
                     "tag is not emitted because its asset_id is not declared in the asset hierarchy");
      continue;
    }
    model.at("tags").push_back(item);
    emitted_tags.emplace(id);
  }
  record_source(tags_artifact, tag_rows.size() - 1U);

  const auto* io_artifact = optional_artifact("io_configuration");
  if (io_artifact != nullptr) {
    const auto io_rows = parse_csv(io_artifact->contents, io_artifact->path.string(), options.limits);
    const auto io_headers = header_map(io_rows, io_artifact->path.string(), options.limits);
    require_columns(io_headers, {"id", "tag_id", "signal_type", "address", "channel"}, io_artifact->path.string());
    check_row_width(io_rows, io_artifact->path.string(), options.limits);
    for (std::size_t index = 1U; index < io_rows.size(); ++index) {
      const auto path = io_artifact->path.filename().string() + "[" + std::to_string(index) + "]";
      const auto id = cell(io_rows[index], io_headers, "id", path, options.limits);
      const auto tag_id = cell(io_rows[index], io_headers, "tag_id", path, options.limits);
      if (!valid_identity(id)) {
        fail("invalid_identity", path + ".id", "I/O record id is invalid");
      }
      if (!emitted_tags.contains(tag_id)) {
        add_unresolved(report, "unknown_io_tag", path,
                       "I/O metadata references a tag not declared in controller_tags");
      }
    }
    report.at("counts")["io"] = io_rows.size() - 1U;
    record_source(*io_artifact, io_rows.size() - 1U);
  }

  const auto& alarms_artifact = find_artifact("alarm_definitions");
  const auto alarm_rows = parse_csv(alarms_artifact.contents, alarms_artifact.path.string(), options.limits);
  const auto alarm_headers = header_map(alarm_rows, alarms_artifact.path.string(), options.limits);
  require_columns(alarm_headers, {"id", "asset_id", "name", "tag_id", "operator", "threshold", "severity"},
                  alarms_artifact.path.string());
  check_row_width(alarm_rows, alarms_artifact.path.string(), options.limits);
  for (std::size_t index = 1U; index < alarm_rows.size(); ++index) {
    const auto path = alarms_artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(alarm_rows[index], alarm_headers, "id", path, options.limits);
    const auto owner = cell(alarm_rows[index], alarm_headers, "asset_id", path, options.limits);
    const auto tag_id = cell(alarm_rows[index], alarm_headers, "tag_id", path, options.limits);
    if (!valid_identity(id) || !alarms.emplace(id).second) {
      fail("invalid_identity", path + ".id", "alarm id is invalid or duplicated");
    }
    const auto tag = std::find_if(model.at("tags").begin(), model.at("tags").end(),
                                  [&](const Json& candidate) {
                                    return candidate.at("id").get<std::string>() == tag_id;
                                  });
    if (!assets.contains(owner) || tag == model.at("tags").end() ||
        tag->at("asset_id").get<std::string>() != owner) {
      add_unresolved(report, "unknown_alarm_reference", path,
                     "alarm is not emitted because its owner or tag is undeclared");
      continue;
    }
    const auto threshold_text = cell(alarm_rows[index], alarm_headers, "threshold", path, options.limits);
    double threshold = 0.0;
    const auto parsed = std::from_chars(threshold_text.data(), threshold_text.data() + threshold_text.size(),
                                        threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != threshold_text.data() + threshold_text.size() ||
        !std::isfinite(threshold)) {
      fail("invalid_number", path + ".threshold", "alarm threshold is invalid");
    }
    const auto operator_value = cell(alarm_rows[index], alarm_headers, "operator", path, options.limits);
    auto severity_value = cell(alarm_rows[index], alarm_headers, "severity", path, options.limits);
    std::transform(severity_value.begin(), severity_value.end(), severity_value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    static const std::set<std::string> operators{">", ">=", "<", "<=", "==", "!="};
    static const std::set<std::string> severities{"low", "medium", "high", "critical"};
    if (!operators.contains(operator_value) || !severities.contains(severity_value)) {
      add_unresolved(report, "unsupported_alarm_definition", path,
                     "alarm is not emitted because its operator or severity is unsupported");
      continue;
    }
    model.at("alarms").push_back(Json{{"id", id},
                                       {"asset_id", owner},
                                       {"name", cell(alarm_rows[index], alarm_headers, "name", path, options.limits)},
                                       {"tag_id", tag_id},
                                       {"operator", operator_value},
                                       {"threshold", threshold},
                                       {"severity", severity_value}});
  }
  record_source(alarms_artifact, alarm_rows.size() - 1U);

  auto parse_metadata = [&](std::string_view kind, std::string_view count_key,
                            std::initializer_list<std::string_view> required) {
    const auto* artifact = optional_artifact(kind);
    if (artifact == nullptr) {
      return;
    }
    const auto records = records_array(parse_json(artifact->contents, artifact->path.string(), options.limits),
                                       artifact->path.string(), options.limits);
    std::unordered_set<std::string> record_ids;
    for (std::size_t index = 0; index < records.size(); ++index) {
      const auto path = artifact->path.filename().string() + "[" + std::to_string(index) + "]";
      if (!records.at(index).is_object()) {
        fail("invalid_record", path, "metadata record must be an object");
      }
      allowed_keys(records.at(index), required, path);
      for (const auto field : required) {
        require_string(records.at(index), field, path, options.limits.max_string_bytes, field == "description");
      }
      const auto record_id = records.at(index).at("id").get<std::string>();
      if (!valid_identity(record_id) || !record_ids.emplace(record_id).second) {
        fail("invalid_identity", path + ".id", "metadata record id is invalid or duplicated");
      }
      if (records.at(index).contains("asset_id") &&
          !assets.contains(records.at(index).at("asset_id").get<std::string>())) {
        add_unresolved(report, "unknown_metadata_asset", path,
                       "metadata references an asset absent from the explicit hierarchy");
      }
    }
    report.at("counts")[std::string(count_key)] = records.size();
    record_source(*artifact, records.size());
  };
  parse_metadata("communications", "communications", {"id", "protocol", "endpoint", "security_mode"});
  parse_metadata("machine_documents", "documents", {"id", "title", "path", "media_type"});

  sort_by_id(model.at("assets"));
  sort_by_id(model.at("relationships"));
  sort_by_id(model.at("tags"));
  sort_by_id(model.at("alarms"));
  std::sort(report.at("artifacts").begin(), report.at("artifacts").end(), [](const Json& left, const Json& right) {
    return left.at("id").get<std::string>() < right.at("id").get<std::string>();
  });
  report["counts"]["assets"] = model.at("assets").size();
  report["counts"]["relationships"] = model.at("relationships").size();
  report["counts"]["tags"] = model.at("tags").size();
  report["counts"]["alarms"] = model.at("alarms").size();
  report["status"] = report.at("errors").empty()
                         ? (report.at("unresolved").empty() ? "ok" : "unresolved")
                         : "error";
  model["metadata"] = Json{{"provenance", Json{{"schema", "engineering-bundle/1"},
                                                 {"manifest", manifest_path.filename().generic_string()},
                                                 {"artifacts", report.at("artifacts")}}}};
  return Json{{"model", std::move(model)}, {"report", std::move(report)}};
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
  const auto output = import_impl(manifest, options);
  return Result{output.at("model"), output.at("report")};
}

}  /* namespace context_hmi::context_import */
