#pragma once

/**
 * Shared model fixtures and JSON search helpers for the native test targets.
 *
 * Contract: base_model() returns a model that validate_model accepts, and an overview task
 * anchored on "tank-a" resolves to a ready view with one component and one
 * dependencies. Tests mutate a copy of this model to express one change at a time, so a
 * failure names one cause. This header defines no test cases and must not be registered
 * as a test source.
 */

#include <cstdint>
#include <string>

#include "context_hmi/engine.hpp"

namespace context_hmi_test {

using context_hmi::Json;

inline Json source_reference(const std::string &identifier) {
    return Json{{"server", "external-source"},
                {"namespace_uri", "urn:context-hmi:source"},
                {"identifier", identifier}};
}

inline Json make_tag(const std::string &id, const std::string &owner, const std::string &name,
                     const std::string &role, const std::string &data_type,
                     const std::string &unit, const std::string &identifier) {
    return Json{{"id", id},         {"asset_id", owner},
                {"name", name},     {"role", role},
                {"data_type", data_type}, {"unit", unit},
                {"source", source_reference(identifier)}};
}

inline Json make_asset(const std::string &id, const std::string &name, const std::string &kind) {
    return Json{{"id", id}, {"name", name}, {"kind", kind}};
}

/**
 * One tank fed by one declared pump, plus an unrelated second tank and second pump so
 * that a wrong owner or a guessed relationship has somewhere visible to go.
 */
inline Json base_model() {
    return Json{
        {"model_id", "station-a"},
        {"revision", 1},
        {"name", "Station A"},
        {"assets", Json::array({Json{{"id", "tank-a"},
                                     {"name", "Tank A"},
                                     {"kind", "tank"},
                                     {"aliases", Json::array({"alpha tank"})}},
                                make_asset("tank-b", "Tank B", "tank"),
                                make_asset("pump-a", "Pump A", "pump"),
                                make_asset("pump-b", "Pump B", "pump")})},
        {"relationships",
         Json::array(
             {Json{{"id", "feed-1"}, {"from", "pump-a"}, {"to", "tank-a"}, {"kind", "feeds"}}})},
        {"tags",
         Json::array({make_tag("tank-a.level", "tank-a", "Level", "level", "number", "%",
                               "TankA.Level"),
                      make_tag("pump-a.flow", "pump-a", "Flow", "flow", "number", "m3/h",
                               "PumpA.Flow"),
                      make_tag("pump-a.state", "pump-a", "State", "operating_state", "string", "",
                               "PumpA.State"),
                      make_tag("pump-b.flow", "pump-b", "Flow", "flow", "number", "m3/h",
                               "PumpB.Flow"),
                      make_tag("pump-b.state", "pump-b", "State", "operating_state", "string", "",
                               "PumpB.State")})},
        {"alarms", Json::array({Json{{"id", "alarm-a"},
                                     {"asset_id", "tank-a"},
                                     {"name", "High level"},
                                     {"tag_id", "tank-a.level"},
                                     {"operator", ">="},
                                     {"threshold", 90},
                                     {"severity", "high"}}})}};
}

inline Json overview_task(std::uint64_t revision = 1) {
    return Json{{"kind", "overview"},
                {"anchor_asset_id", "tank-a"},
                {"model_revision", revision},
                {"original_request", "Show the overview for Tank A"}};
}

inline Json overview_view(const Json &model, std::uint64_t revision = 1) {
    return context_hmi::resolve_task(model, overview_task(revision));
}

/** Return the first object in array whose string field key equals value, or nullptr. */
inline const Json *find_by(const Json &array, const char *key, const std::string &value) {
    if (!array.is_array())
        return nullptr;
    for (const auto &item : array) {
        if (!item.is_object() || !item.contains(key) || !item.at(key).is_string())
            continue;
        if (item.at(key).get<std::string>() == value)
            return &item;
    }
    return nullptr;
}

inline bool has_field_value(const Json &array, const char *key, const std::string &value) {
    return find_by(array, key, value) != nullptr;
}

/** Read a string field, or return an empty string when it is absent or not a string. */
inline std::string string_or_empty(const Json &object, const char *key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string())
        return std::string();
    return object.at(key).get<std::string>();
}

}  /* namespace context_hmi_test */
