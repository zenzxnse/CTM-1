#pragma once

#include "context_hmi/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace context_hmi::internal {

constexpr std::size_t kMaxString = 256;
constexpr std::size_t kMaxPrompt = 8192;
constexpr std::size_t kMaxAssets = 256;
constexpr std::size_t kMaxRelationships = 512;
constexpr std::size_t kMaxTags = 2048;
constexpr std::size_t kMaxAlarms = 2048;
constexpr std::size_t kMaxAliases = 32;
constexpr std::size_t kMaxComponents = 4096;

struct ModelIndex {
    const Json *model = nullptr;
    std::unordered_map<std::string, const Json *> assets;
    std::unordered_map<std::string, const Json *> relationships;
    std::unordered_map<std::string, const Json *> tags;
    std::unordered_map<std::string, const Json *> alarms;
};

[[noreturn]] void invalid(const std::string &details);
[[noreturn]] void invalid_task(const std::string &details);
[[noreturn]] void invalid_view(const std::string &details);

bool is_nonnegative_integer(const Json &value);
std::string lower(std::string value);
std::string task_string(const Json &object, const char *key, const std::string &where,
                        std::size_t max = kMaxString);
ModelIndex index_model(const Json &model);
std::string asset_name(const ModelIndex &index, const std::string &id);

Json tag_binding_snapshot(const Json &tag);
std::string tag_fingerprint(const Json &tag);
Json dependency_for(const Json &tag);
Json alarm_definition_snapshot(const Json &alarm);
std::string alarm_definition_fingerprint(const Json &alarm);
Json dependency_for_alarm(const Json &alarm, const Json &tag);
Json issue(std::string code, std::string message, std::string severity = "warning");

Json resolve_task_impl(const ModelIndex &index, const Json &task);
Json reconcile_view_impl(const ModelIndex &index, const Json &previous_view);

}  /* namespace context_hmi::internal */
