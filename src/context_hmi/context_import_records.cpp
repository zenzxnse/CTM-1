#include "context_hmi/context_import_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace context_hmi::context_import::detail {

void parse_assets(const Artifact& artifact, ImportState& state) {
  const auto records = records_array(parse_json(artifact.contents, artifact.path.string(), *state.limits),
                                     artifact.path.string(), *state.limits);
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto& item = records.at(index);
    if (!item.is_object()) {
      fail("invalid_record", path, "asset record must be an object");
    }
    allowed_keys(item, {"id", "name", "kind", "aliases", "description"}, path);
    const auto id = string_field(item, "id", path, state.limits->max_string_bytes);
    if (!valid_identity(id) || !state.assets.emplace(id).second) {
      fail("invalid_identity", path + ".id", "asset id is invalid or duplicated");
    }
    const auto name = string_field(item, "name", path, state.limits->max_string_bytes);
    const auto kind = string_field(item, "kind", path, state.limits->max_string_bytes);
    Json asset{{"id", id}, {"name", name}, {"kind", kind}};
    if (item.contains("aliases")) {
      if (!item.at("aliases").is_array() || item.at("aliases").size() > 32U) {
        fail("invalid_field", path + ".aliases", "aliases must be a bounded array");
      }
      asset["aliases"] = Json::array();
      for (const auto& alias : item.at("aliases")) {
        if (!alias.is_string() || alias.get<std::string>().empty() ||
            alias.get<std::string>().size() > std::min(state.limits->max_string_bytes, std::size_t{256}) ||
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
                                                               state.limits->max_string_bytes, true)}};
    }
    state.model.at("assets").push_back(std::move(asset));
  }
  record_source(state, artifact, records.size());
}

void parse_relationships(const Artifact& artifact, ImportState& state) {
  const auto rows = parse_csv(artifact.contents, artifact.path.string(), *state.limits);
  const auto headers = header_map(rows, artifact.path.string(), *state.limits);
  require_columns(headers, {"id", "from", "to", "kind"}, artifact.path.string());
  check_row_width(rows, artifact.path.string(), *state.limits);
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(rows[index], headers, "id", path, *state.limits);
    const auto from = cell(rows[index], headers, "from", path, *state.limits);
    const auto to = cell(rows[index], headers, "to", path, *state.limits);
    const auto kind = cell(rows[index], headers, "kind", path, *state.limits);
    if (!valid_identity(id) || !state.relationships.emplace(id).second) {
      fail("invalid_identity", path + ".id", "relationship id is invalid or duplicated");
    }
    if (from == to || !state.assets.contains(from) || !state.assets.contains(to)) {
      add_unresolved(state.report, "unknown_relationship_endpoint", path,
                     "relationship is not emitted because its endpoints are undeclared or identical");
      continue;
    }
    state.model.at("relationships").push_back(Json{{"id", id}, {"from", from}, {"to", to}, {"kind", kind}});
  }
  record_source(state, artifact, rows.size() - 1U);
}

void parse_tags(const Artifact& artifact, ImportState& state) {
  const auto rows = parse_csv(artifact.contents, artifact.path.string(), *state.limits);
  const auto headers = header_map(rows, artifact.path.string(), *state.limits);
  require_columns(headers, {"id", "asset_id", "name", "role", "data_type", "unit", "server",
                             "namespace_uri", "identifier"},
                  artifact.path.string());
  check_row_width(rows, artifact.path.string(), *state.limits);
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(rows[index], headers, "id", path, *state.limits);
    const auto owner = cell(rows[index], headers, "asset_id", path, *state.limits);
    if (!valid_identity(id) || !state.tags.emplace(id).second) {
      fail("invalid_identity", path + ".id", "tag id is invalid or duplicated");
    }
    auto item = Json{{"id", id},
                     {"asset_id", owner},
                     {"name", cell(rows[index], headers, "name", path, *state.limits)},
                     {"role", cell(rows[index], headers, "role", path, *state.limits)},
                     {"data_type", cell(rows[index], headers, "data_type", path, *state.limits)},
                     {"unit", cell(rows[index], headers, "unit", path, *state.limits, false)},
                     {"source", Json{{"server", cell(rows[index], headers, "server", path, *state.limits)},
                                      {"namespace_uri", cell(rows[index], headers, "namespace_uri", path, *state.limits)},
                                      {"identifier", cell(rows[index], headers, "identifier", path, *state.limits)}}}};
    auto data_type = item.at("data_type").get<std::string>();
    std::transform(data_type.begin(), data_type.end(), data_type.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    item["data_type"] = data_type;
    static const std::set<std::string> supported_data_types{
        "number", "float", "double", "integer", "boolean", "bool", "string"};
    if (!supported_data_types.contains(data_type)) {
      add_unresolved(state.report, "unsupported_tag_type", path,
                     "tag is not emitted because its data_type is not supported by the native model");
      continue;
    }
    if (!state.assets.contains(owner)) {
      add_unresolved(state.report, "unknown_tag_owner", path,
                     "tag is not emitted because its asset_id is not declared in the asset hierarchy");
      continue;
    }
    state.model.at("tags").push_back(item);
    state.emitted_tags.emplace(id);
  }
  record_source(state, artifact, rows.size() - 1U);
}

void parse_io(const Artifact& artifact, ImportState& state) {
  const auto rows = parse_csv(artifact.contents, artifact.path.string(), *state.limits);
  const auto headers = header_map(rows, artifact.path.string(), *state.limits);
  require_columns(headers, {"id", "tag_id", "signal_type", "address", "channel"}, artifact.path.string());
  check_row_width(rows, artifact.path.string(), *state.limits);
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(rows[index], headers, "id", path, *state.limits);
    const auto tag_id = cell(rows[index], headers, "tag_id", path, *state.limits);
    if (!valid_identity(id)) {
      fail("invalid_identity", path + ".id", "I/O record id is invalid");
    }
    if (!state.emitted_tags.contains(tag_id)) {
      add_unresolved(state.report, "unknown_io_tag", path,
                     "I/O metadata references a tag not declared in controller_tags");
    }
  }
  state.report.at("counts")["io"] = rows.size() - 1U;
  record_source(state, artifact, rows.size() - 1U);
}

void parse_alarms(const Artifact& artifact, ImportState& state) {
  const auto rows = parse_csv(artifact.contents, artifact.path.string(), *state.limits);
  const auto headers = header_map(rows, artifact.path.string(), *state.limits);
  require_columns(headers, {"id", "asset_id", "name", "tag_id", "operator", "threshold", "severity"},
                  artifact.path.string());
  check_row_width(rows, artifact.path.string(), *state.limits);
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    const auto id = cell(rows[index], headers, "id", path, *state.limits);
    const auto owner = cell(rows[index], headers, "asset_id", path, *state.limits);
    const auto tag_id = cell(rows[index], headers, "tag_id", path, *state.limits);
    if (!valid_identity(id) || !state.alarms.emplace(id).second) {
      fail("invalid_identity", path + ".id", "alarm id is invalid or duplicated");
    }
    const auto tag = std::find_if(state.model.at("tags").begin(), state.model.at("tags").end(),
                                  [&](const Json& candidate) {
                                    return candidate.at("id").get<std::string>() == tag_id;
                                  });
    if (!state.assets.contains(owner) || tag == state.model.at("tags").end() ||
        tag->at("asset_id").get<std::string>() != owner) {
      add_unresolved(state.report, "unknown_alarm_reference", path,
                     "alarm is not emitted because its owner or tag is undeclared");
      continue;
    }
    const auto threshold_text = cell(rows[index], headers, "threshold", path, *state.limits);
    double threshold = 0.0;
    const auto parsed = std::from_chars(threshold_text.data(), threshold_text.data() + threshold_text.size(), threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != threshold_text.data() + threshold_text.size() ||
        !std::isfinite(threshold)) {
      fail("invalid_number", path + ".threshold", "alarm threshold is invalid");
    }
    const auto operator_value = cell(rows[index], headers, "operator", path, *state.limits);
    auto severity_value = cell(rows[index], headers, "severity", path, *state.limits);
    std::transform(severity_value.begin(), severity_value.end(), severity_value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    static const std::set<std::string> operators{">", ">=", "<", "<=", "==", "!="};
    static const std::set<std::string> severities{"low", "medium", "high", "critical"};
    if (!operators.contains(operator_value) || !severities.contains(severity_value)) {
      add_unresolved(state.report, "unsupported_alarm_definition", path,
                     "alarm is not emitted because its operator or severity is unsupported");
      continue;
    }
    state.model.at("alarms").push_back(Json{{"id", id},
                                              {"asset_id", owner},
                                              {"name", cell(rows[index], headers, "name", path, *state.limits)},
                                              {"tag_id", tag_id},
                                              {"operator", operator_value},
                                              {"threshold", threshold},
                                              {"severity", severity_value}});
  }
  record_source(state, artifact, rows.size() - 1U);
}

void parse_metadata(const Artifact& artifact, std::string_view count_key, ImportState& state,
                    std::initializer_list<std::string_view> required) {
  const auto records = records_array(parse_json(artifact.contents, artifact.path.string(), *state.limits),
                                     artifact.path.string(), *state.limits);
  std::unordered_set<std::string> record_ids;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto path = artifact.path.filename().string() + "[" + std::to_string(index) + "]";
    if (!records.at(index).is_object()) {
      fail("invalid_record", path, "metadata record must be an object");
    }
    allowed_keys(records.at(index), required, path);
    for (const auto field : required) {
      require_string(records.at(index), field, path, state.limits->max_string_bytes, field == "description");
    }
    const auto record_id = records.at(index).at("id").get<std::string>();
    if (!valid_identity(record_id) || !record_ids.emplace(record_id).second) {
      fail("invalid_identity", path + ".id", "metadata record id is invalid or duplicated");
    }
    if (records.at(index).contains("asset_id") &&
        !state.assets.contains(records.at(index).at("asset_id").get<std::string>())) {
      add_unresolved(state.report, "unknown_metadata_asset", path,
                     "metadata references an asset absent from the explicit hierarchy");
    }
  }
  state.report.at("counts")[std::string(count_key)] = records.size();
  record_source(state, artifact, records.size());
}

}  /* namespace context_hmi::context_import::detail */
