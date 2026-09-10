#include "context_hmi/engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace context_hmi {
namespace {

constexpr std::size_t kMaxString = 256;
constexpr std::size_t kMaxPrompt = 8192;
constexpr std::size_t kMaxAssets = 256;
constexpr std::size_t kMaxRelationships = 512;
constexpr std::size_t kMaxTags = 2048;
constexpr std::size_t kMaxAlarms = 2048;
constexpr std::size_t kMaxAliases = 32;
constexpr std::size_t kMaxComponents = 4096;

using Object = std::unordered_map<std::string, const Json *>;

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
    return value.get_ref<const std::string &>();
}

std::string task_string(const Json &object, const char *key, const std::string &where,
                        std::size_t max = kMaxString) {
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

std::string normalized(std::string value) {
    value = lower(std::move(value));
    std::string output;
    output.reserve(value.size());
    bool space = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            space = !output.empty();
        } else {
            if (space && !output.empty())
                output.push_back(' ');
            output.push_back(static_cast<char>(c));
            space = false;
        }
    }
    return output;
}

bool is_integer(const Json &value) {
    return value.is_number_integer() && !value.is_number_unsigned();
}
bool is_nonnegative_integer(const Json &value) {
    return value.is_number_unsigned() || (is_integer(value) && value.get<std::int64_t>() >= 0);
}

struct ModelIndex {
    const Json *model = nullptr;
    std::unordered_map<std::string, const Json *> assets;
    std::unordered_map<std::string, const Json *> relationships;
    std::unordered_map<std::string, const Json *> tags;
    std::unordered_map<std::string, const Json *> alarms;
};

const Json &source_field(const Json &tag, const std::string &where) {
    const Json &source = required(tag, "source", where);
    if (!source.is_object())
        invalid(where + ".source must be an object");
    string_field(source, "server", where + ".source");
    string_field(source, "namespace_uri", where + ".source");
    string_field(source, "identifier", where + ".source");
    return source;
}

ModelIndex index_model(const Json &model) {
    if (!model.is_object())
        invalid("model must be an object");
    string_field(model, "model_id", "model");
    if (!model.contains("revision"))
        invalid("model requires 'revision'");
    const Json &revision = model.at("revision");
    if (!is_nonnegative_integer(revision) ||
        revision.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        invalid("model.revision must be a non-negative 32-bit integer");
    string_field(model, "name", "model");
    bounded_array(model, "assets", "model", kMaxAssets);
    bounded_array(model, "relationships", "model", kMaxRelationships);
    bounded_array(model, "tags", "model", kMaxTags);
    bounded_array(model, "alarms", "model", kMaxAlarms);

    ModelIndex index;
    index.model = &model;
    for (std::size_t i = 0; i < model.at("assets").size(); ++i) {
        const Json &asset = model.at("assets").at(i);
        const std::string where = "model.assets[" + std::to_string(i) + "]";
        if (!asset.is_object())
            invalid(where + " must be an object");
        const std::string id = string_field(asset, "id", where);
        string_field(asset, "name", where);
        string_field(asset, "kind", where);
        if (asset.contains("aliases")) {
            if (!asset.at("aliases").is_array() || asset.at("aliases").size() > kMaxAliases)
                invalid(where + ".aliases exceeds limit");
            for (std::size_t j = 0; j < asset.at("aliases").size(); ++j) {
                if (!asset.at("aliases").at(j).is_string() ||
                    asset.at("aliases").at(j).get<std::string>().empty() ||
                    asset.at("aliases").at(j).get<std::string>().size() > kMaxString)
                    invalid(where + ".aliases[" + std::to_string(j) +
                            "] must be a non-empty string");
            }
        }
        if (!index.assets.emplace(id, &asset).second)
            invalid("duplicate asset id '" + id + "'");
    }
    for (std::size_t i = 0; i < model.at("relationships").size(); ++i) {
        const Json &rel = model.at("relationships").at(i);
        const std::string where = "model.relationships[" + std::to_string(i) + "]";
        if (!rel.is_object())
            invalid(where + " must be an object");
        const std::string id = string_field(rel, "id", where);
        const std::string from = string_field(rel, "from", where);
        const std::string to = string_field(rel, "to", where);
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
        if (!tag.is_object())
            invalid(where + " must be an object");
        const std::string id = string_field(tag, "id", where);
        const std::string owner = string_field(tag, "asset_id", where);
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
        if (!alarm.is_object())
            invalid(where + " must be an object");
        const std::string id = string_field(alarm, "id", where);
        const std::string owner = string_field(alarm, "asset_id", where);
        const std::string tag_id = string_field(alarm, "tag_id", where);
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
    std::uint64_t hash = 1469598103934665603ULL;
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
    // This is a display/debug fingerprint only. Reconciliation compares the canonical snapshot
    // below.
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

Json component_for(const Json &tag, const std::string &kind, const std::string &label,
                   const std::string &reason) {
    Json result{{"id", tag.at("id")},     {"kind", kind},
                {"label", label},         {"asset_id", tag.at("asset_id")},
                {"tag_id", tag.at("id")}, {"role", tag.at("role")},
                {"unit", tag.at("unit")}, {"reason", reason}};
    result["dependency_fingerprints"] = Json::array({tag_fingerprint(tag)});
    return result;
}

Json alarm_component_for(const Json &alarm, const Json &tag) {
    return Json{{"id", alarm.at("id")},
                {"kind", "alarm"},
                {"label", alarm.at("name")},
                {"asset_id", alarm.at("asset_id")},
                {"alarm_id", alarm.at("id")},
                {"tag_id", alarm.at("tag_id")},
                {"role", tag.at("role")},
                {"unit", tag.at("unit")},
                {"reason", "Declared alarm threshold"},
                {"dependency_fingerprints",
                 Json::array({tag_fingerprint(tag), alarm_definition_fingerprint(alarm)})}};
}

bool role_is(const Json &tag, std::initializer_list<std::string_view> roles) {
    const std::string role = lower(tag.at("role").get<std::string>());
    for (const auto candidate : roles)
        if (role == candidate)
            return true;
    return false;
}

std::vector<const Json *> tags_for(const ModelIndex &index, const std::string &asset_id,
                                   std::initializer_list<std::string_view> roles) {
    std::vector<const Json *> output;
    for (const auto &[id, tag] : index.tags)
        if (tag->at("asset_id") == asset_id && role_is(*tag, roles))
            output.push_back(tag);
    std::sort(output.begin(), output.end(), [](const Json *a, const Json *b) {
        return a->at("id").get<std::string>() < b->at("id").get<std::string>();
    });
    return output;
}

Json issue(std::string code, std::string message, std::string severity = "warning") {
    return Json{{"code", std::move(code)},
                {"message", std::move(message)},
                {"severity", std::move(severity)}};
}

bool contains_phrase(const std::string &text, const std::string &phrase) {
    if (phrase.empty())
        return false;
    std::size_t position = text.find(phrase);
    while (position != std::string::npos) {
        const bool left_ok =
            position == 0 || !std::isalnum(static_cast<unsigned char>(text[position - 1]));
        const std::size_t end = position + phrase.size();
        const bool right_ok =
            end == text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
        if (left_ok && right_ok)
            return true;
        position = text.find(phrase, position + 1);
    }
    return false;
}

std::vector<std::string> candidate_assets(const ModelIndex &index, const std::string &prompt) {
    const std::string input = normalized(prompt);
    std::vector<std::string> candidates;
    for (const auto &[id, asset] : index.assets) {
        std::vector<std::string> names{id, asset->at("name").get<std::string>()};
        if (asset->contains("aliases"))
            for (const auto &alias : asset->at("aliases"))
                names.push_back(alias.get<std::string>());
        bool matched = false;
        for (const auto &name : names) {
            const std::string n = normalized(name);
            if (input == n || contains_phrase(input, n)) {
                matched = true;
                break;
            }
        }
        if (matched)
            candidates.push_back(id);
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

std::string task_kind(const std::string &prompt) {
    const std::string input = normalized(prompt);
    const bool alarms = input.find("alarm") != std::string::npos;
    const bool filling =
        input.find("fill") != std::string::npos || input.find("transfer") != std::string::npos;
    const bool overview = input.find("overview") != std::string::npos ||
                          input.find("status") != std::string::npos ||
                          input.find("summary") != std::string::npos;
    const int intents =
        static_cast<int>(alarms) + static_cast<int>(filling) + static_cast<int>(overview);
    if (intents != 1)
        return {};
    if (alarms)
        return "alarms";
    if (filling)
        return "filling";
    return "overview";
}

Json make_task(const std::string &kind, const std::string &anchor, const Json &model_id,
               std::uint64_t revision, const std::string &prompt) {
    Json task{{"kind", kind},
              {"model_id", model_id},
              {"model_revision", revision},
              {"original_request", prompt}};
    if (!anchor.empty())
        task["anchor_asset_id"] = anchor;
    return task;
}

Json resolve_filling(const ModelIndex &index, const Json &task) {
    const std::string anchor = task.contains("anchor_asset_id")
                                   ? task.at("anchor_asset_id").get<std::string>()
                                   : std::string();
    Json view{{"view_id", anchor.empty() ? "view-filling" : "view-" + anchor + "-filling"},
              {"model_id", index.model->at("model_id")},
              {"model_revision", index.model->at("revision")},
              {"status", "ready"},
              {"title", anchor.empty() ? "Filling" : asset_name(index, anchor) + " - Filling"},
              {"task", task},
              {"components", Json::array()},
              {"issues", Json::array()},
              {"dependencies", Json::array()}};
    if (anchor.empty()) {
        view["status"] = "needs-review";
        view["issues"].push_back(
            issue("missing-anchor", "Filling requires an anchor tank or equipment asset", "error"));
        return view;
    }
    const auto level_tags = tags_for(index, anchor, {"level"});
    if (level_tags.size() == 1) {
        const Json &level = *level_tags.front();
        view["components"].push_back(component_for(
            level, "gauge", asset_name(index, anchor) + " level", "Tank level during filling"));
        view["dependencies"].push_back(dependency_for(level));
    } else {
        view["status"] = "needs-review";
        view["issues"].push_back(issue(
            level_tags.empty() ? "missing-level" : "ambiguous-level",
            level_tags.empty() ? "No declared level tag exists for " + asset_name(index, anchor)
                               : "Multiple declared level tags exist for " +
                                     asset_name(index, anchor) + "; choose one",
            "error"));
    }
    std::vector<const Json *> feeds;
    for (const auto &[id, rel] : index.relationships) {
        if (lower(rel->at("kind").get<std::string>()) == "feeds" &&
            rel->at("to").get<std::string>() == anchor)
            feeds.push_back(rel);
    }
    std::sort(feeds.begin(), feeds.end(), [](const Json *a, const Json *b) {
        return a->at("id").get<std::string>() < b->at("id").get<std::string>();
    });
    if (feeds.empty()) {
        view["status"] = "needs-review";
        view["issues"].push_back(issue(
            "missing-feeding-equipment",
            "No declared feeding relationship targets " + asset_name(index, anchor), "error"));
    }
    for (const Json *rel : feeds) {
        const std::string equipment = rel->at("from").get<std::string>();
        const auto flow_tags = tags_for(index, equipment, {"flow"});
        const auto state_tags = tags_for(index, equipment, {"operating_state", "state", "status"});
        if (flow_tags.size() != 1) {
            view["status"] = "needs-review";
            view["issues"].push_back(
                issue(flow_tags.empty() ? "missing-feed-flow" : "ambiguous-feed-flow",
                      flow_tags.empty()
                          ? "Declared feed " + asset_name(index, equipment) + " has no flow tag"
                          : "Declared feed " + asset_name(index, equipment) +
                                " has multiple flow tags; choose one",
                      "error"));
        }
        if (state_tags.size() != 1) {
            view["status"] = "needs-review";
            view["issues"].push_back(
                issue(state_tags.empty() ? "missing-feed-state" : "ambiguous-feed-state",
                      state_tags.empty() ? "Declared feed " + asset_name(index, equipment) +
                                               " has no operating state tag"
                                         : "Declared feed " + asset_name(index, equipment) +
                                               " has multiple operating state tags; choose one",
                      "error"));
        }
        const Json *flow = flow_tags.size() == 1 ? flow_tags.front() : nullptr;
        const Json *state = state_tags.size() == 1 ? state_tags.front() : nullptr;
        if (flow) {
            auto component = component_for(*flow, "value", asset_name(index, equipment) + " flow",
                                           "Declared feeding equipment flow during filling");
            component["relationship_id"] = rel->at("id");
            component["relationship_kind"] = rel->at("kind");
            component["relationship_from"] = rel->at("from");
            component["relationship_to"] = rel->at("to");
            view["components"].push_back(std::move(component));
            view["dependencies"].push_back(dependency_for(*flow));
        }
        if (state) {
            auto component =
                component_for(*state, "status", asset_name(index, equipment) + " state",
                              "Declared feeding equipment operating state during filling");
            component["relationship_id"] = rel->at("id");
            component["relationship_kind"] = rel->at("kind");
            component["relationship_from"] = rel->at("from");
            component["relationship_to"] = rel->at("to");
            view["components"].push_back(std::move(component));
            view["dependencies"].push_back(dependency_for(*state));
        }
    }
    std::vector<const Json *> scoped_alarms;
    for (const auto &[id, alarm] : index.alarms)
        if (alarm->at("asset_id").get<std::string>() == anchor)
            scoped_alarms.push_back(alarm);
    std::sort(scoped_alarms.begin(), scoped_alarms.end(), [](const Json *a, const Json *b) {
        return a->at("id").get<std::string>() < b->at("id").get<std::string>();
    });
    for (const Json *alarm : scoped_alarms) {
        const Json &alarm_tag = *index.tags.at(alarm->at("tag_id").get<std::string>());
        view["components"].push_back(alarm_component_for(*alarm, alarm_tag));
        view["dependencies"].push_back(dependency_for_alarm(*alarm, alarm_tag));
    }
    return view;
}

Json resolve_overview(const ModelIndex &index, const Json &task) {
    const std::string anchor = task.contains("anchor_asset_id")
                                   ? task.at("anchor_asset_id").get<std::string>()
                                   : std::string();
    Json view{{"view_id", anchor.empty() ? "view-overview" : "view-" + anchor + "-overview"},
              {"model_id", index.model->at("model_id")},
              {"model_revision", index.model->at("revision")},
              {"status", "ready"},
              {"title", anchor.empty() ? "Overview" : asset_name(index, anchor) + " - Overview"},
              {"task", task},
              {"components", Json::array()},
              {"issues", Json::array()},
              {"dependencies", Json::array()}};
    std::vector<std::string> assets;
    if (!anchor.empty())
        assets.push_back(anchor);
    else
        for (const auto &[id, a] : index.assets)
            assets.push_back(id);
    std::sort(assets.begin(), assets.end());
    for (const auto &asset : assets) {
        std::vector<const Json *> overview_tags;
        for (const auto &[id, tag] : index.tags)
            if (tag->at("asset_id") == asset)
                overview_tags.push_back(tag);
        std::sort(overview_tags.begin(), overview_tags.end(), [](const Json *a, const Json *b) {
            return a->at("id").get<std::string>() < b->at("id").get<std::string>();
        });
        for (const Json *tag : overview_tags) {
            const std::string role = lower(tag->at("role").get<std::string>());
            const std::string kind =
                role == "level"
                    ? "gauge"
                    : (role == "status" || role == "state" || role == "operating_state" ? "status"
                                                                                        : "value");
            view["components"].push_back(component_for(
                *tag, kind, asset_name(index, asset) + " " + tag->at("name").get<std::string>(),
                "Declared tag in machine overview"));
            view["dependencies"].push_back(dependency_for(*tag));
        }
    }
    if (view["components"].empty()) {
        view["status"] = "needs-review";
        view["issues"].push_back(issue(
            "missing-overview-tags", "No declared tags are available for this overview", "error"));
    }
    return view;
}

Json resolve_alarms(const ModelIndex &index, const Json &task) {
    const std::string anchor = task.contains("anchor_asset_id")
                                   ? task.at("anchor_asset_id").get<std::string>()
                                   : std::string();
    Json view{{"view_id", anchor.empty() ? "view-alarms" : "view-" + anchor + "-alarms"},
              {"model_id", index.model->at("model_id")},
              {"model_revision", index.model->at("revision")},
              {"status", "ready"},
              {"title", anchor.empty() ? "Alarms" : asset_name(index, anchor) + " - Alarms"},
              {"task", task},
              {"components", Json::array()},
              {"issues", Json::array()},
              {"dependencies", Json::array()}};
    std::vector<const Json *> alarms;
    for (const auto &[id, alarm] : index.alarms)
        if (anchor.empty() || alarm->at("asset_id").get<std::string>() == anchor)
            alarms.push_back(alarm);
    std::sort(alarms.begin(), alarms.end(), [](const Json *a, const Json *b) {
        return a->at("id").get<std::string>() < b->at("id").get<std::string>();
    });
    for (const Json *alarm : alarms) {
        const Json &tag = *index.tags.at(alarm->at("tag_id").get<std::string>());
        view["components"].push_back(alarm_component_for(*alarm, tag));
        view["dependencies"].push_back(dependency_for_alarm(*alarm, tag));
    }
    if (view["components"].empty()) {
        view["status"] = "needs-review";
        view["issues"].push_back(
            issue("no-alarms", "No declared alarms are available for this scope", "info"));
    }
    return view;
}

Json resolve_task_impl(const ModelIndex &index, const Json &task) {
    if (!task.is_object())
        invalid_task("task must be an object");
    static const std::set<std::string> allowed_task_fields{"kind", "model_id", "anchor_asset_id",
                                                           "model_revision", "original_request"};
    for (const auto &item : task.items())
        if (!allowed_task_fields.count(item.key()))
            invalid_task("task contains unsupported field '" + item.key() + "'");
    const std::string kind = task_string(task, "kind", "task", 32);
    if (kind != "filling" && kind != "overview" && kind != "alarms")
        invalid_task("unsupported task kind '" + kind + "'");
    if (!task.contains("model_revision") || !is_nonnegative_integer(task.at("model_revision")))
        invalid_task("task.model_revision must be a non-negative integer");
    if (task.at("model_revision") != index.model->at("revision"))
        invalid_task("task.model_revision does not match the active model");
    if (task.contains("model_id") &&
        (!task.at("model_id").is_string() || task.at("model_id") != index.model->at("model_id")))
        invalid_task("task.model_id does not match the active model");
    const std::string original = task_string(task, "original_request", "task", kMaxPrompt);
    (void)original;
    if (task.contains("anchor_asset_id")) {
        if (!task.at("anchor_asset_id").is_string() ||
            task.at("anchor_asset_id").get<std::string>().empty())
            invalid_task("task.anchor_asset_id must be a non-empty string");
        if (!index.assets.count(task.at("anchor_asset_id").get<std::string>()))
            invalid_task("task.anchor_asset_id references an unknown asset");
    } else if (kind == "filling") {
        // An absent anchor remains a structured needs-review result for ordinary request context.
    }
    Json effective_task = task;
    if (!effective_task.contains("model_id"))
        effective_task["model_id"] = index.model->at("model_id");
    if (kind == "filling")
        return resolve_filling(index, effective_task);
    if (kind == "overview")
        return resolve_overview(index, effective_task);
    return resolve_alarms(index, effective_task);
}

bool compare_alarm(double value, const std::string &op, double threshold) {
    if (op == ">")
        return value > threshold;
    if (op == ">=")
        return value >= threshold;
    if (op == "<")
        return value < threshold;
    if (op == "<=")
        return value <= threshold;
    if (op == "==")
        return value == threshold;
    return value != threshold;
}

} // namespace

DomainError::DomainError(std::string code_value, std::string details_value)
    : std::runtime_error(code_value + ": " + details_value), code(std::move(code_value)),
      details(std::move(details_value)) {}

Json validate_model(const Json &model) {
    const ModelIndex index = index_model(model);
    return Json{{"valid", true},
                {"model_id", model.at("model_id")},
                {"revision", model.at("revision")},
                {"asset_count", index.assets.size()},
                {"relationship_count", index.relationships.size()},
                {"tag_count", index.tags.size()},
                {"alarm_count", index.alarms.size()}};
}

Json interpret_request(const Json &model, const std::string &prompt) {
    const ModelIndex index = index_model(model);
    if (prompt.empty() || prompt.size() > kMaxPrompt)
        throw DomainError("invalid_request", "prompt must be non-empty and at most " +
                                                 std::to_string(kMaxPrompt) + " characters");
    const std::string kind = task_kind(prompt);
    const std::vector<std::string> candidates = candidate_assets(index, prompt);
    Json result{{"interpreter", "rules"},
                {"prompt", prompt},
                {"status", "clarification"},
                {"message", "Specify one supported task and equipment asset"},
                {"candidates", Json::array()},
                {"supported_tasks", Json::array({"filling", "overview", "alarms"})}};
    for (const auto &id : candidates)
        result["candidates"].push_back(Json{{"asset_id", id}, {"name", asset_name(index, id)}});
    if (kind.empty()) {
        result["message"] = "The rules interpreter supports filling, overview, and alarms";
        return result;
    }
    if (candidates.size() > 1) {
        result["message"] = "The equipment reference is ambiguous; choose one candidate";
        return result;
    }
    const std::string normalized_prompt = normalized(prompt);
    const bool mentions_equipment = normalized_prompt.find("tank") != std::string::npos ||
                                    normalized_prompt.find("pump") != std::string::npos ||
                                    normalized_prompt.find("valve") != std::string::npos ||
                                    normalized_prompt.find("equipment") != std::string::npos ||
                                    normalized_prompt.find("asset") != std::string::npos;
    if (candidates.empty() && mentions_equipment) {
        result["message"] =
            "The equipment reference does not match a declared asset; choose one candidate";
        for (const auto &[id, asset] : index.assets)
            result["candidates"].push_back(Json{{"asset_id", id}, {"name", asset->at("name")}});
        return result;
    }
    if (kind == "filling" && candidates.empty()) {
        result["message"] = "Filling requires an equipment asset; choose one candidate";
        for (const auto &[id, asset] : index.assets)
            result["candidates"].push_back(Json{{"asset_id", id}, {"name", asset->at("name")}});
        return result;
    }
    result["status"] = "ready";
    result.erase("message");
    result["task"] = make_task(kind, candidates.empty() ? std::string() : candidates.front(),
                               index.model->at("model_id"), index.model->at("revision"), prompt);
    return result;
}

Json resolve_task(const Json &model, const Json &task) {
    const ModelIndex index = index_model(model);
    return resolve_task_impl(index, task);
}

Json reconcile_view(const Json &model, const Json &previous_view) {
    const ModelIndex index = index_model(model);
    if (!previous_view.is_object())
        invalid_view("previous_view must be an object");
    if (!previous_view.contains("task") || !previous_view.at("task").is_object())
        invalid_view("previous_view.task is required");
    if (!previous_view.contains("components") || !previous_view.at("components").is_array() ||
        previous_view.at("components").size() > kMaxComponents)
        invalid_view("previous_view.components is invalid or exceeds limit");
    const std::string model_id = index.model->at("model_id").get<std::string>();
    if (previous_view.contains("model_id") &&
        (!previous_view.at("model_id").is_string() ||
         previous_view.at("model_id").get<std::string>() != model_id))
        throw DomainError("model_mismatch",
                          "a view from another model cannot be reconciled by revision alone");
    const Json &old_task = previous_view.at("task");
    if (old_task.contains("model_id") && (!old_task.at("model_id").is_string() ||
                                          old_task.at("model_id").get<std::string>() != model_id))
        throw DomainError("model_mismatch", "task belongs to another model");

    Json new_task = old_task;
    new_task["model_id"] = model_id;
    new_task["model_revision"] = index.model->at("revision");
    Json fresh = resolve_task_impl(index, new_task);
    Json result = fresh;
    result["task"] = new_task;
    result["changes"] = previous_view.value("changes", Json::array());
    if (!result["changes"].is_array())
        result["changes"] = Json::array();
    result["issues"] = previous_view.value("issues", Json::array());
    if (!result["issues"].is_array())
        result["issues"] = Json::array();

    auto add_issue_once = [&](const Json &item) {
        for (const auto &existing : result["issues"])
            if (existing == item)
                return;
        result["issues"].push_back(item);
    };
    auto add_change_once = [&](const Json &item) {
        for (const auto &existing : result["changes"])
            if (existing == item)
                return;
        result["changes"].push_back(item);
    };
    std::unordered_map<std::string, Json> old_dependencies;
    std::unordered_map<std::string, Json> old_alarm_dependencies;
    if (previous_view.contains("dependencies") && previous_view.at("dependencies").is_array()) {
        for (const auto &dep : previous_view.at("dependencies")) {
            if (!dep.is_object() || !dep.contains("tag_id") || !dep.at("tag_id").is_string())
                continue;
            const std::string tag_id = dep.at("tag_id").get<std::string>();
            if (dep.contains("alarm_id") && dep.at("alarm_id").is_string())
                old_alarm_dependencies[dep.at("alarm_id").get<std::string>()] = dep;
            else
                old_dependencies[tag_id] = dep;
        }
    }
    std::unordered_set<std::string> conflicted_tags;
    std::unordered_set<std::string> removed_tags;
    for (const auto &[tag_id, old_dep] : old_dependencies) {
        if (!index.tags.count(tag_id)) {
            removed_tags.insert(tag_id);
            add_change_once(Json{{"kind", "removed"},
                                 {"tag_id", tag_id},
                                 {"message", "Previously bound tag was removed"}});
            add_issue_once(issue("removed-tag",
                                 "Previously bound tag " + tag_id + " is unavailable", "error"));
            continue;
        }
        const Json &current = *index.tags.at(tag_id);
        bool binding_differs = false;
        if (old_dep.contains("binding")) {
            binding_differs = old_dep.at("binding") != tag_binding_snapshot(current);
            // A client cannot forge a top-level summary independently of its snapshot.
            if (!old_dep.at("binding").is_object())
                binding_differs = true;
            else {
                if (old_dep.contains("owner") &&
                    old_dep.at("owner") != old_dep.at("binding").value("owner", Json()))
                    binding_differs = true;
                if (old_dep.contains("role") &&
                    old_dep.at("role") != old_dep.at("binding").value("role", Json()))
                    binding_differs = true;
                if (old_dep.contains("unit") &&
                    old_dep.at("unit") != old_dep.at("binding").value("unit", Json()))
                    binding_differs = true;
                if (old_dep.contains("source") &&
                    old_dep.at("source") != old_dep.at("binding").value("source", Json()))
                    binding_differs = true;
            }
        } else if (old_dep.contains("owner") && old_dep.contains("role") &&
                   old_dep.contains("unit") && old_dep.contains("source")) {
            binding_differs = old_dep.at("owner") != current.at("asset_id") ||
                              old_dep.at("role") != current.at("role") ||
                              old_dep.at("unit") != current.at("unit") ||
                              old_dep.at("source") != current.at("source");
            if (old_dep.contains("data_type"))
                binding_differs =
                    binding_differs || old_dep.at("data_type") != current.at("data_type");
        } else {
            // An unstructured dependency cannot prove that the meaning stayed the same.
            binding_differs = true;
        }
        if (binding_differs) {
            conflicted_tags.insert(tag_id);
            add_change_once(
                Json{{"kind", "conflict"},
                     {"tag_id", tag_id},
                     {"previous_fingerprint", old_dep.value("fingerprint", "")},
                     {"current_fingerprint", tag_fingerprint(current)},
                     {"message", "The same tag ID changed owner, role, unit, type, or source"}});
            add_issue_once(
                issue("binding-conflict",
                      "Tag " + tag_id + " retained its ID but changed meaning; review the binding",
                      "error"));
        }
    }
    std::unordered_set<std::string> conflicted_alarms;
    std::unordered_set<std::string> removed_alarms;
    for (const auto &[alarm_id, old_dep] : old_alarm_dependencies) {
        const auto alarm_it = index.alarms.find(alarm_id);
        if (alarm_it == index.alarms.end()) {
            if (old_dep.contains("tag_id") && old_dep.at("tag_id").is_string() &&
                !index.tags.count(old_dep.at("tag_id").get<std::string>()))
                removed_tags.insert(old_dep.at("tag_id").get<std::string>());
            removed_alarms.insert(alarm_id);
            add_change_once(Json{{"kind", "removed-alarm"},
                                 {"alarm_id", alarm_id},
                                 {"message", "Previously bound alarm was removed"}});
            add_issue_once(issue("removed-alarm",
                                 "Previously bound alarm " + alarm_id + " is unavailable",
                                 "error"));
            continue;
        }
        const Json &current = *alarm_it->second;
        const std::string alarm_tag_id = current.at("tag_id").get<std::string>();
        if (!index.tags.count(alarm_tag_id)) {
            removed_tags.insert(alarm_tag_id);
            add_issue_once(issue("removed-tag",
                                 "Previously bound alarm tag " + alarm_tag_id + " is unavailable",
                                 "error"));
        } else if (old_dep.contains("binding") &&
                   old_dep.at("binding") != tag_binding_snapshot(*index.tags.at(alarm_tag_id))) {
            conflicted_tags.insert(alarm_tag_id);
            add_change_once(
                Json{{"kind", "conflict"},
                     {"tag_id", alarm_tag_id},
                     {"message", "The alarm's tag ID retained its identity but changed meaning"}});
            add_issue_once(issue(
                "binding-conflict",
                "Alarm tag " + alarm_tag_id + " changed meaning; review the binding", "error"));
        }
        const Json current_definition = alarm_definition_snapshot(current);
        bool differs =
            old_dep.contains("alarm_definition")
                ? old_dep.at("alarm_definition") != current_definition
                : old_dep.value("alarm_owner", "") != current.at("asset_id").get<std::string>() ||
                      old_dep.value("alarm_name", "") != current.at("name").get<std::string>() ||
                      old_dep.value("tag_id", "") != current.at("tag_id").get<std::string>() ||
                      old_dep.value("operator", "") != current.at("operator").get<std::string>() ||
                      old_dep.value("threshold", Json()) != current.at("threshold") ||
                      old_dep.value("severity", "") != current.at("severity").get<std::string>();
        if (old_dep.contains("alarm_definition") && old_dep.at("alarm_definition").is_object()) {
            if (old_dep.contains("alarm_owner") &&
                old_dep.at("alarm_owner") != old_dep.at("alarm_definition").value("owner", Json()))
                differs = true;
            if (old_dep.contains("alarm_name") &&
                old_dep.at("alarm_name") != old_dep.at("alarm_definition").value("name", Json()))
                differs = true;
            if (old_dep.contains("operator") &&
                old_dep.at("operator") != old_dep.at("alarm_definition").value("operator", Json()))
                differs = true;
            if (old_dep.contains("threshold") &&
                old_dep.at("threshold") !=
                    old_dep.at("alarm_definition").value("threshold", Json()))
                differs = true;
            if (old_dep.contains("severity") &&
                old_dep.at("severity") != old_dep.at("alarm_definition").value("severity", Json()))
                differs = true;
        }
        if (differs) {
            conflicted_alarms.insert(alarm_id);
            add_change_once(Json{
                {"kind", "alarm-conflict"},
                {"alarm_id", alarm_id},
                {"message",
                 "The same alarm ID changed its owner, threshold, operator, severity, or tag"}});
            add_issue_once(issue("alarm-conflict",
                                 "Alarm " + alarm_id + " changed meaning; review the binding",
                                 "error"));
        }
    }

    std::unordered_set<std::string> removed_relationships;
    std::unordered_set<std::string> changed_relationships;
    for (const auto &old_component : previous_view.at("components")) {
        if (!old_component.is_object() || !old_component.contains("relationship_id") ||
            !old_component.at("relationship_id").is_string())
            continue;
        const std::string rel_id = old_component.at("relationship_id").get<std::string>();
        if (!index.relationships.count(rel_id)) {
            removed_relationships.insert(rel_id);
            add_change_once(Json{{"kind", "removed-relationship"},
                                 {"relationship_id", rel_id},
                                 {"message", "Previously declared feed relationship was removed"}});
            add_issue_once(issue(
                "removed-relationship",
                "Previously declared feed relationship " + rel_id + " is unavailable", "error"));
            continue;
        }
        const Json &current_rel = *index.relationships.at(rel_id);
        const bool differs =
            old_component.value("relationship_kind", "") !=
                current_rel.at("kind").get<std::string>() ||
            old_component.value("relationship_from", "") !=
                current_rel.at("from").get<std::string>() ||
            old_component.value("relationship_to", "") != current_rel.at("to").get<std::string>();
        if (differs) {
            changed_relationships.insert(rel_id);
            add_change_once(
                Json{{"kind", "relationship-conflict"},
                     {"relationship_id", rel_id},
                     {"message", "The same relationship ID changed endpoints or kind"}});
            add_issue_once(issue("relationship-conflict",
                                 "Relationship " + rel_id + " changed meaning; review the binding",
                                 "error"));
        }
    }

    Json merged = Json::array();
    std::unordered_set<std::string> emitted;
    for (const auto &old_component : previous_view.at("components")) {
        if (!old_component.is_object() || !old_component.contains("id") ||
            !old_component.at("id").is_string())
            continue;
        const std::string id = old_component.at("id").get<std::string>();
        Json preserved = old_component;
        const std::string tag_id = old_component.value("tag_id", "");
        const std::string alarm_id = old_component.value("alarm_id", "");
        const std::string rel_id = old_component.value("relationship_id", "");
        const bool prior_blocked = old_component.value("status", "") == "conflict" ||
                                   old_component.value("status", "") == "unavailable";
        if (!tag_id.empty() && removed_tags.count(tag_id)) {
            preserved["status"] = "unavailable";
            preserved["reason"] = "Previously bound tag was removed from the model";
        }
        if (!tag_id.empty() && conflicted_tags.count(tag_id)) {
            preserved["status"] = "conflict";
            preserved["reason"] = "Same-ID binding changed; no silent rebinding was performed";
        }
        if (!alarm_id.empty() && removed_alarms.count(alarm_id)) {
            preserved["status"] = "unavailable";
            preserved["reason"] = "Previously bound alarm was removed from the model";
        }
        if (!alarm_id.empty() && conflicted_alarms.count(alarm_id)) {
            preserved["status"] = "conflict";
            preserved["reason"] =
                "Same-ID alarm definition changed; no silent rebinding was performed";
        }
        if (!rel_id.empty() && removed_relationships.count(rel_id)) {
            preserved["status"] = "unavailable";
            preserved["reason"] = "Declared relationship was removed; no replacement was inferred";
        }
        if (!rel_id.empty() && changed_relationships.count(rel_id)) {
            preserved["status"] = "conflict";
            preserved["reason"] = "Same-ID relationship changed; no silent rebinding was performed";
        }
        if (prior_blocked || conflicted_tags.count(tag_id) || removed_tags.count(tag_id) ||
            conflicted_alarms.count(alarm_id) || removed_alarms.count(alarm_id) ||
            removed_relationships.count(rel_id) || changed_relationships.count(rel_id)) {
            merged.push_back(std::move(preserved));
            emitted.insert(id);
            continue;
        }
        bool found = false;
        for (const auto &current_component : fresh["components"])
            if (current_component.is_object() && current_component.value("id", "") == id) {
                merged.push_back(current_component);
                emitted.insert(id);
                found = true;
                break;
            }
        if (!found) {
            preserved["status"] = "unavailable";
            preserved["reason"] = "Previously bound component is no longer declared";
            merged.push_back(std::move(preserved));
            emitted.insert(id);
        }
    }
    for (const auto &current_component : fresh["components"]) {
        const std::string id = current_component.value("id", "");
        if (!emitted.count(id))
            merged.push_back(current_component);
    }
    result["components"] = std::move(merged);
    if (previous_view.contains("dependencies") && previous_view.at("dependencies").is_array()) {
        Json merged_dependencies = Json::array();
        std::unordered_set<std::string> emitted_dependencies;
        for (const auto &old_dep : previous_view.at("dependencies")) {
            if (!old_dep.is_object() || !old_dep.contains("tag_id") ||
                !old_dep.at("tag_id").is_string())
                continue;
            const std::string tag_id = old_dep.at("tag_id").get<std::string>();
            const std::string alarm_id = old_dep.value("alarm_id", "");
            const std::string key = alarm_id.empty() ? "tag:" + tag_id : "alarm:" + alarm_id;
            const bool blocked = removed_tags.count(tag_id) || conflicted_tags.count(tag_id) ||
                                 (!alarm_id.empty() && (removed_alarms.count(alarm_id) ||
                                                        conflicted_alarms.count(alarm_id)));
            if (blocked) {
                merged_dependencies.push_back(old_dep);
                emitted_dependencies.insert(key);
                continue;
            }
            bool found_dependency = false;
            for (const auto &current_dep : fresh["dependencies"]) {
                const std::string current_key = current_dep.value("alarm_id", "").empty()
                                                    ? "tag:" + current_dep.value("tag_id", "")
                                                    : "alarm:" + current_dep.value("alarm_id", "");
                if (current_key == key) {
                    merged_dependencies.push_back(current_dep);
                    found_dependency = true;
                    break;
                }
            }
            if (!found_dependency)
                merged_dependencies.push_back(old_dep);
            emitted_dependencies.insert(key);
        }
        for (const auto &current_dep : fresh["dependencies"]) {
            const std::string key = current_dep.value("alarm_id", "").empty()
                                        ? "tag:" + current_dep.value("tag_id", "")
                                        : "alarm:" + current_dep.value("alarm_id", "");
            if (!emitted_dependencies.count(key))
                merged_dependencies.push_back(current_dep);
        }
        result["dependencies"] = std::move(merged_dependencies);
    }
    // Keep fresh issues while retaining prior issues, and make the state explicit.
    for (const auto &item : fresh["issues"])
        add_issue_once(item);
    bool prior_conflict = previous_view.value("status", "") == "conflict";
    if (previous_view.contains("changes") && previous_view.at("changes").is_array())
        for (const auto &change : previous_view.at("changes"))
            if (change.value("kind", "") == "conflict" ||
                change.value("kind", "") == "relationship-conflict" ||
                change.value("kind", "") == "alarm-conflict")
                prior_conflict = true;
    for (const auto &component : previous_view.at("components"))
        if (component.value("status", "") == "conflict")
            prior_conflict = true;
    if (!conflicted_tags.empty() || !conflicted_alarms.empty() || !changed_relationships.empty() ||
        prior_conflict)
        result["status"] = "conflict";
    else if (!removed_tags.empty() || !removed_alarms.empty() || !removed_relationships.empty() ||
             !result["issues"].empty() || previous_view.value("status", "") == "needs-review")
        result["status"] = "needs-review";
    else
        result["status"] = "ready";
    result["reconciled"] = true;
    return result;
}

Json make_telemetry(const Json &model, std::uint64_t tick, bool running) {
    const ModelIndex index = index_model(model);
    Json result{{"model_id", index.model->at("model_id")},
                {"model_revision", index.model->at("revision")},
                {"tick", tick},
                {"quality", "good"},
                {"values", Json::object()},
                {"alarms", Json::array()}};
    for (const auto &[id, tag] : index.tags) {
        const std::string type = lower(tag->at("data_type").get<std::string>());
        const std::uint64_t seed = std::stoull(fnv_hex(id), nullptr, 16);
        const std::uint64_t phase = running ? tick : 0;
        if (type == "number" || type == "float" || type == "double" || type == "integer") {
            const double value =
                static_cast<double>((seed % 1001 + (phase * 37ULL) % 1001) % 1001) / 10.0;
            result["values"][id] = Json{{"value", value},
                                        {"quality", running ? "good" : "paused"},
                                        {"unit", tag->at("unit")},
                                        {"timestamp_ms", tick * 1000ULL}};
            if (type == "integer")
                result["values"][id]["value"] = static_cast<std::int64_t>(value);
        } else if (type == "boolean" || type == "bool") {
            result["values"][id] = Json{{"value", running && (((seed + phase) & 1ULL) != 0)},
                                        {"quality", running ? "good" : "paused"},
                                        {"unit", tag->at("unit")},
                                        {"timestamp_ms", tick * 1000ULL}};
        } else {
            result["values"][id] = Json{{"value", running ? "running" : "stopped"},
                                        {"quality", running ? "good" : "paused"},
                                        {"unit", tag->at("unit")},
                                        {"timestamp_ms", tick * 1000ULL}};
        }
    }
    for (const auto &[id, alarm] : index.alarms) {
        const std::string tag_id = alarm->at("tag_id").get<std::string>();
        const Json &raw = result["values"][tag_id];
        if (!raw.is_object() || !raw.contains("value") || !raw.at("value").is_number())
            continue;
        const double value = raw.at("value").get<double>();
        const double threshold = alarm->at("threshold").get<double>();
        const bool active =
            compare_alarm(value, alarm->at("operator").get<std::string>(), threshold);
        result["alarms"].push_back(Json{{"id", id},
                                        {"alarm_id", id},
                                        {"active", active},
                                        {"asset_id", alarm->at("asset_id")},
                                        {"tag_id", tag_id},
                                        {"name", alarm->at("name")},
                                        {"severity", alarm->at("severity")},
                                        {"value", value},
                                        {"threshold", alarm->at("threshold")}});
    }
    std::sort(result["alarms"].begin(), result["alarms"].end(), [](const Json &a, const Json &b) {
        return a.at("alarm_id").get<std::string>() < b.at("alarm_id").get<std::string>();
    });
    return result;
}

} // namespace context_hmi
