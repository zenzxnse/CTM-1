#include "context_hmi/engine_internal.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace context_hmi::internal {

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

Json issue(std::string code, std::string message, std::string severity) {
    return Json{{"code", std::move(code)},
                {"message", std::move(message)},
                {"severity", std::move(severity)}};
}

std::set<std::string> requested_roles(const Json &task) {
    std::set<std::string> roles;
    if (!task.contains("measurement_roles"))
        return roles;
    const auto &requested = task.at("measurement_roles");
    if (!requested.is_array() || requested.empty() || requested.size() > 16)
        invalid_task("task.measurement_roles must be a non-empty array of at most 16 roles");
    for (const auto &role : requested) {
        if (!role.is_string() || role.get<std::string>().empty() ||
            role.get<std::string>().size() > kMaxString)
            invalid_task("task.measurement_roles contains an invalid role");
        roles.insert(role.get<std::string>());
    }
    if (roles.size() != requested.size())
        invalid_task("task.measurement_roles must not contain duplicates");
    return roles;
}

Json responsive_layout(const Json &task) {
    Json layout{{"strategy", "responsive-grid"},
                {"reflow", true},
                {"minimum_component_width_px", 220},
                {"size_class", "unspecified"}};
    if (!task.contains("client"))
        return layout;
    const auto &client = task.at("client");
    if (!client.is_object())
        invalid_task("task.client must be an object");
    static const std::set<std::string> allowed{"width_px", "height_px", "size_class",
                                               "density", "reduced_motion"};
    for (const auto &field : client.items())
        if (!allowed.contains(field.key()))
            invalid_task("task.client contains unsupported field '" + field.key() + "'");
    const auto dimension = [&](const char *name, std::uint64_t minimum,
                               std::uint64_t maximum) -> std::optional<std::uint64_t> {
        if (!client.contains(name))
            return std::nullopt;
        if (!is_nonnegative_integer(client.at(name)))
            invalid_task(std::string("task.client.") + name + " must be an integer");
        const auto value = client.at(name).get<std::uint64_t>();
        if (value < minimum || value > maximum)
            invalid_task(std::string("task.client.") + name + " is out of range");
        return value;
    };
    const auto width = dimension("width_px", 240, 7680);
    const auto height = dimension("height_px", 160, 4320);
    if (width) {
        layout["viewport_width_px"] = *width;
        if (*width < 600) {
            layout["size_class"] = "compact";
            layout["minimum_component_width_px"] = 156;
        } else if (*width < 1200) {
            layout["size_class"] = "standard";
            layout["minimum_component_width_px"] = 200;
        } else {
            layout["size_class"] = "wide";
        }
    }
    if (height)
        layout["viewport_height_px"] = *height;
    if (client.contains("size_class")) {
        if (!client.at("size_class").is_string() ||
            (client.at("size_class") != "small" &&
             client.at("size_class") != "medium" &&
             client.at("size_class") != "large"))
            invalid_task("task.client.size_class must be small, medium, or large");
        layout["reported_size_class"] = client.at("size_class");
    }
    if (client.contains("density")) {
        if (!client.at("density").is_string() ||
            (client.at("density") != "compact" && client.at("density") != "comfortable"))
            invalid_task("task.client.density must be compact or comfortable");
        layout["density"] = client.at("density");
    } else {
        layout["density"] = width && *width < 600 ? "compact" : "comfortable";
    }
    if (client.contains("reduced_motion")) {
        if (!client.at("reduced_motion").is_boolean())
            invalid_task("task.client.reduced_motion must be boolean");
        layout["reduced_motion"] = client.at("reduced_motion");
    }
    return layout;
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
    const auto roles = requested_roles(task);
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
            if (tag->at("asset_id") == asset &&
                (roles.empty() || roles.contains(tag->at("role").get<std::string>())))
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
    const auto roles = requested_roles(task);
    std::vector<const Json *> alarms;
    for (const auto &[id, alarm] : index.alarms) {
        (void)id;
        if (!anchor.empty() && alarm->at("asset_id").get<std::string>() != anchor) {
            continue;
        }
        const auto tag = index.tags.at(alarm->at("tag_id").get<std::string>());
        if (roles.empty() || roles.contains(tag->at("role").get<std::string>())) {
            alarms.push_back(alarm);
        }
    }
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
    static const std::set<std::string> allowed_task_fields{
        "kind", "model_id", "anchor_asset_id", "measurement_roles", "client",
        "model_revision", "original_request"};
    for (const auto &item : task.items())
        if (!allowed_task_fields.count(item.key()))
            invalid_task("task contains unsupported field '" + item.key() + "'");
    const std::string kind = task_string(task, "kind", "task", 32);
    if (kind != "overview" && kind != "alarms")
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
    }
    Json effective_task = task;
    if (!effective_task.contains("model_id"))
        effective_task["model_id"] = index.model->at("model_id");
    Json view;
    if (kind == "overview")
        view = resolve_overview(index, effective_task);
    else
        view = resolve_alarms(index, effective_task);
    view["layout"] = responsive_layout(effective_task);
    return view;
}

}  /* namespace context_hmi::internal */
