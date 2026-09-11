#include "context_hmi/engine_internal.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace context_hmi::internal {

Json reconcile_view_impl(const ModelIndex &index, const Json &previous_view) {
    if (!previous_view.is_object())
        invalid_view("previous_view must be an object");
    if (!previous_view.contains("task") || !previous_view.at("task").is_object())
        invalid_view("previous_view.task is required");
    if (!previous_view.contains("components") || !previous_view.at("components").is_array() ||
        previous_view.at("components").size() > kMaxComponents)
        invalid_view("previous_view.components is invalid or exceeds limit");
    if (previous_view.contains("dependencies") &&
        (!previous_view.at("dependencies").is_array() ||
         previous_view.at("dependencies").size() > kMaxComponents))
        invalid_view("previous_view.dependencies is invalid or exceeds limit");
    if (previous_view.contains("issues") &&
        (!previous_view.at("issues").is_array() ||
         previous_view.at("issues").size() > kMaxComponents))
        invalid_view("previous_view.issues is invalid or exceeds limit");
    if (previous_view.contains("changes") &&
        (!previous_view.at("changes").is_array() ||
         previous_view.at("changes").size() > kMaxComponents))
        invalid_view("previous_view.changes is invalid or exceeds limit");
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
            /** A top-level summary cannot override its canonical binding snapshot. */
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
            /** An unstructured dependency cannot prove unchanged meaning. */
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
    if (merged.size() > kMaxComponents)
        invalid_view("reconciled component count exceeds limit");
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
    /** Keep current and prior issues while making the resulting state explicit. */
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

}  /* namespace context_hmi::internal */

