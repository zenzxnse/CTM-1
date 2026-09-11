#include "context_hmi/engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace context_hmi::internal {



[[noreturn]] void invalid(const std::string &details) {
    throw DomainError("invalid_model", details);
}
[[noreturn]] void invalid_task(const std::string &details) {
    throw DomainError("invalid_task", details);
}
[[noreturn]] void invalid_view(const std::string &details) {
    throw DomainError("invalid_view", details);
}

const Json &required(const Json &object, const char *key, const std::string &where) {
    if (!object.is_object() || !object.contains(key))
        invalid(where + " requires '" + key + "'");
    return object.at(key);
}

const std::string &string_field(const Json &object, const char *key, const std::string &where,
                                std::size_t max = kMaxString) {
    const Json &value = required(object, key, where);
    if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
        value.get_ref<const std::string &>().size() > max)
        invalid(where + "." + key + " must be a non-empty string of at most " +
                std::to_string(max) + " characters");
    if (std::any_of(value.get_ref<const std::string &>().begin(),
                    value.get_ref<const std::string &>().end(), [](unsigned char character) {
                        return std::iscntrl(character) != 0;
                    }))
        invalid(where + "." + key + " must not contain control characters");
    return value.get_ref<const std::string &>();
}

bool valid_identity(const std::string &value) {
    if (value.empty() || value.size() > kMaxString ||
        !std::isalnum(static_cast<unsigned char>(value.front())))
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '.' ||
               character == '-';
    });
}

const std::string &identity_field(const Json &object, const char *key, const std::string &where) {
    const std::string &value = string_field(object, key, where);
    if (!valid_identity(value))
        invalid(where + "." + key + " must be an ASCII identifier");
    return value;
}

void allow_only(const Json &object, std::initializer_list<std::string_view> names,
                const std::string &where) {
    if (!object.is_object())
        invalid(where + " must be an object");
    for (const auto &item : object.items()) {
        if (std::none_of(names.begin(), names.end(), [&](std::string_view name) {
                return item.key() == name;
            }))
            invalid(where + " contains unsupported field '" + item.key() + "'");
    }
}

std::string task_string(const Json &object, const char *key, const std::string &where,
                        std::size_t max) {
    if (!object.is_object() || !object.contains(key))
        invalid_task(where + " requires '" + key + "'");
    const Json &value = object.at(key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
        value.get_ref<const std::string &>().size() > max)
        invalid_task(where + "." + key + " must be a non-empty string of at most " +
                     std::to_string(max) + " characters");
    return value.get<std::string>();
}

void unit_field(const Json &object, const char *key, const std::string &where) {
    const Json &value = required(object, key, where);
    if (!value.is_string() || value.get_ref<const std::string &>().size() > kMaxString)
        invalid(where + "." + key + " must be a string of at most " + std::to_string(kMaxString) +
                " characters");
    if (std::any_of(value.get_ref<const std::string &>().begin(),
                    value.get_ref<const std::string &>().end(), [](unsigned char character) {
                        return std::iscntrl(character) != 0;
                    }))
        invalid(where + "." + key + " must not contain control characters");
}

void bounded_array(const Json &object, const char *key, const std::string &where, std::size_t max) {
    const Json &value = required(object, key, where);
    if (!value.is_array() || value.size() > max)
        invalid(where + "." + key + " must be an array with at most " + std::to_string(max) +
                " items");
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_integer(const Json &value) {
    return value.is_number_integer() && !value.is_number_unsigned();
}
bool is_nonnegative_integer(const Json &value) {
    return value.is_number_unsigned() || (is_integer(value) && value.get<std::int64_t>() >= 0);
}

const Json &source_field(const Json &tag, const std::string &where) {
    const Json &source = required(tag, "source", where);
    allow_only(source, {"server", "namespace_uri", "identifier"}, where + ".source");
    string_field(source, "server", where + ".source");
    string_field(source, "namespace_uri", where + ".source");
    string_field(source, "identifier", where + ".source");
    return source;
}

ModelIndex index_model(const Json &model) {
    allow_only(model,
               {"model_id", "revision", "name", "assets", "relationships", "tags", "alarms",
                "scenario_name", "description", "metadata"},
               "model");
    identity_field(model, "model_id", "model");
    if (!model.contains("revision"))
        invalid("model requires 'revision'");
    const Json &revision = model.at("revision");
    if (!is_nonnegative_integer(revision) ||
        revision.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        invalid("model.revision must be a non-negative 32-bit integer");
    string_field(model, "name", "model");
    if (model.contains("scenario_name"))
        string_field(model, "scenario_name", "model");
    if (model.contains("description"))
        string_field(model, "description", "model", 2048);
    if (model.contains("metadata") &&
        (!model.at("metadata").is_object() || model.at("metadata").dump().size() > 16 * 1024))
        invalid("model.metadata must be an object of at most 16 KiB");
    bounded_array(model, "assets", "model", kMaxAssets);
    bounded_array(model, "relationships", "model", kMaxRelationships);
    bounded_array(model, "tags", "model", kMaxTags);
    bounded_array(model, "alarms", "model", kMaxAlarms);

    ModelIndex index;
    index.model = &model;
    for (std::size_t i = 0; i < model.at("assets").size(); ++i) {
        const Json &asset = model.at("assets").at(i);
        const std::string where = "model.assets[" + std::to_string(i) + "]";
        allow_only(asset, {"id", "name", "kind", "aliases", "metadata"}, where);
        const std::string id = identity_field(asset, "id", where);
        string_field(asset, "name", where);
        string_field(asset, "kind", where);
        if (asset.contains("aliases")) {
            if (!asset.at("aliases").is_array() || asset.at("aliases").size() > kMaxAliases)
                invalid(where + ".aliases exceeds limit");
            for (std::size_t j = 0; j < asset.at("aliases").size(); ++j) {
                if (!asset.at("aliases").at(j).is_string() ||
                    asset.at("aliases").at(j).get<std::string>().empty() ||
                    asset.at("aliases").at(j).get<std::string>().size() > kMaxString ||
                    std::any_of(asset.at("aliases").at(j).get_ref<const std::string &>().begin(),
                                asset.at("aliases").at(j).get_ref<const std::string &>().end(),
                                [](unsigned char character) {
                                    return std::iscntrl(character) != 0;
                                }))
                    invalid(where + ".aliases[" + std::to_string(j) +
                            "] must be a non-empty string");
            }
        }
        if (asset.contains("metadata") &&
            (!asset.at("metadata").is_object() || asset.at("metadata").dump().size() > 4096))
            invalid(where + ".metadata must be an object of at most 4 KiB");
        if (!index.assets.emplace(id, &asset).second)
            invalid("duplicate asset id '" + id + "'");
    }
    for (std::size_t i = 0; i < model.at("relationships").size(); ++i) {
        const Json &rel = model.at("relationships").at(i);
        const std::string where = "model.relationships[" + std::to_string(i) + "]";
        allow_only(rel, {"id", "from", "to", "kind", "metadata"}, where);
        const std::string id = identity_field(rel, "id", where);
        const std::string from = identity_field(rel, "from", where);
        const std::string to = identity_field(rel, "to", where);
        string_field(rel, "kind", where);
        if (!index.assets.count(from) || !index.assets.count(to))
            invalid(where + " references an unknown asset");
        if (from == to)
            invalid(where + " cannot connect an asset to itself");
        if (!index.relationships.emplace(id, &rel).second)
            invalid("duplicate relationship id '" + id + "'");
    }
    for (std::size_t i = 0; i < model.at("tags").size(); ++i) {
        const Json &tag = model.at("tags").at(i);
        const std::string where = "model.tags[" + std::to_string(i) + "]";
        allow_only(tag,
                   {"id", "asset_id", "name", "role", "data_type", "unit", "source",
                    "metadata"},
                   where);
        const std::string id = identity_field(tag, "id", where);
        const std::string owner = identity_field(tag, "asset_id", where);
        string_field(tag, "name", where);
        string_field(tag, "role", where);
        const std::string data_type = lower(string_field(tag, "data_type", where));
        static const std::set<std::string> data_types{"number",  "float", "double", "integer",
                                                      "boolean", "bool",  "string"};
        if (!data_types.count(data_type))
            invalid(where + " has unsupported data_type");
        unit_field(tag, "unit", where);
        if ((lower(tag.at("role").get<std::string>()) == "level" ||
             lower(tag.at("role").get<std::string>()) == "flow") &&
            !((data_type == "number") || (data_type == "float") || (data_type == "double") ||
              (data_type == "integer")))
            invalid(where + " level and flow roles must use a numeric data_type");
        source_field(tag, where);
        if (!index.assets.count(owner))
            invalid(where + " references an unknown asset");
        if (!index.tags.emplace(id, &tag).second)
            invalid("duplicate tag id '" + id + "'");
    }
    static const std::set<std::string> operators{">", ">=", "<", "<=", "==", "!="};
    static const std::set<std::string> severities{"low", "medium", "high", "critical"};
    for (std::size_t i = 0; i < model.at("alarms").size(); ++i) {
        const Json &alarm = model.at("alarms").at(i);
        const std::string where = "model.alarms[" + std::to_string(i) + "]";
        allow_only(alarm,
                   {"id", "asset_id", "name", "tag_id", "operator", "threshold", "severity",
                    "metadata"},
                   where);
        const std::string id = identity_field(alarm, "id", where);
        const std::string owner = identity_field(alarm, "asset_id", where);
        const std::string tag_id = identity_field(alarm, "tag_id", where);
        const std::string op = string_field(alarm, "operator", where, 8);
        const std::string severity = lower(string_field(alarm, "severity", where, 16));
        string_field(alarm, "name", where);
        const Json &threshold = required(alarm, "threshold", where);
        if (!threshold.is_number() || !std::isfinite(threshold.get<double>()))
            invalid(where + ".threshold must be a finite numeric value");
        if (!index.assets.count(owner))
            invalid(where + " references an unknown asset");
        if (!index.tags.count(tag_id))
            invalid(where + " references an unknown tag");
        if (index.tags.at(tag_id)->at("asset_id").get<std::string>() != owner)
            invalid(where + " asset_id does not own tag_id");
        const std::string alarm_data_type =
            lower(index.tags.at(tag_id)->at("data_type").get<std::string>());
        if (alarm_data_type != "number" && alarm_data_type != "float" &&
            alarm_data_type != "double" && alarm_data_type != "integer")
            invalid(where + " tag_id must reference a numeric tag");
        if (!operators.count(op))
            invalid(where + " has unsupported operator");
        if (!severities.count(severity))
            invalid(where + " has unsupported severity");
        if (!index.alarms.emplace(id, &alarm).second)
            invalid("duplicate alarm id '" + id + "'");
    }
    return index;
}

std::string asset_name(const ModelIndex &index, const std::string &id) {
    return index.assets.at(id)->at("name").get<std::string>();
}

std::string fnv_hex(std::string_view input) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : input) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

Json tag_binding_snapshot(const Json &tag) {
    return Json{{"owner", tag.at("asset_id")},
                {"role", tag.at("role")},
                {"data_type", tag.at("data_type")},
                {"unit", tag.at("unit")},
                {"source", tag.at("source")}};
}

std::string tag_fingerprint(const Json &tag) {
    /** This diagnostic fingerprint is never used as identity or authorization evidence. */
    return fnv_hex(tag_binding_snapshot(tag).dump());
}

Json dependency_for(const Json &tag) {
    Json result{{"tag_id", tag.at("id")},
                {"owner", tag.at("asset_id")},
                {"owner_asset_id", tag.at("asset_id")},
                {"role", tag.at("role")},
                {"data_type", tag.at("data_type")},
                {"unit", tag.at("unit")},
                {"source", tag.at("source")},
                {"binding", tag_binding_snapshot(tag)}};
    result["fingerprint"] = tag_fingerprint(tag);
    return result;
}

Json alarm_definition_snapshot(const Json &alarm) {
    return Json{{"owner", alarm.at("asset_id")},      {"name", alarm.at("name")},
                {"tag_id", alarm.at("tag_id")},       {"operator", alarm.at("operator")},
                {"threshold", alarm.at("threshold")}, {"severity", alarm.at("severity")}};
}

std::string alarm_definition_fingerprint(const Json &alarm) {
    return fnv_hex(alarm_definition_snapshot(alarm).dump());
}

Json dependency_for_alarm(const Json &alarm, const Json &tag) {
    Json result = dependency_for(tag);
    result["alarm_id"] = alarm.at("id");
    result["alarm_owner"] = alarm.at("asset_id");
    result["alarm_name"] = alarm.at("name");
    result["operator"] = alarm.at("operator");
    result["threshold"] = alarm.at("threshold");
    result["severity"] = alarm.at("severity");
    result["alarm_definition"] = alarm_definition_snapshot(alarm);
    result["alarm_fingerprint"] = alarm_definition_fingerprint(alarm);
    return result;
}

}  /* namespace context_hmi::internal */
