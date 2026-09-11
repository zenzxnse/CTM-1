#pragma once

#include "context_hmi/context_import.hpp"

#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace context_hmi::context_import::detail {

using Rows = std::vector<std::vector<std::string>>;

struct Artifact {
  std::string id;
  std::string kind;
  std::string format;
  std::filesystem::path path;
  std::string contents;
};

struct ImportState {
  Json model;
  Json report;
  std::unordered_set<std::string> assets;
  std::unordered_set<std::string> relationships;
  std::unordered_set<std::string> tags;
  std::unordered_set<std::string> emitted_tags;
  std::unordered_set<std::string> alarms;
  std::size_t total_records{0};
  const Limits* limits{nullptr};
};

[[noreturn]] void fail(std::string code, const std::string& path, std::string message);
void require_string(const Json& object, std::string_view key, const std::string& path,
                    std::size_t limit, bool allow_empty = false);
std::string string_field(const Json& object, std::string_view key, const std::string& path,
                         std::size_t limit, bool allow_empty = false);
void allowed_keys(const Json& object, std::initializer_list<std::string_view> allowed,
                  const std::string& path);
std::string trim(std::string_view value);
std::string read_bounded(const std::filesystem::path& path, const Limits& limits);
Json parse_json(const std::string& contents, const std::string& path, const Limits& limits);
std::string fingerprint(std::string_view contents);

Rows parse_csv(std::string_view contents, const std::string& path, const Limits& limits);
std::map<std::string, std::size_t> header_map(const Rows& rows, const std::string& path,
                                               const Limits& limits);
std::string cell(const std::vector<std::string>& row,
                 const std::map<std::string, std::size_t>& headers, std::string_view key,
                 const std::string& path, const Limits& limits, bool required = true);
void require_columns(const std::map<std::string, std::size_t>& headers,
                     std::initializer_list<std::string_view> required, const std::string& path);
void check_row_width(const Rows& rows, const std::string& path, const Limits& limits);

bool valid_identity(std::string_view value);
Json records_array(const Json& value, const std::string& path, const Limits& limits);
void add_unresolved(Json& report, std::string code, std::string path, std::string message);
std::filesystem::path contained_path(const std::filesystem::path& root, const std::string& relative,
                                     const std::string& path);
Json source_record(const Artifact& artifact, std::size_t records);
void record_source(ImportState& state, const Artifact& artifact, std::size_t records);
void sort_by_id(Json& array);

void parse_assets(const Artifact& artifact, ImportState& state);
void parse_relationships(const Artifact& artifact, ImportState& state);
void parse_tags(const Artifact& artifact, ImportState& state);
void parse_io(const Artifact& artifact, ImportState& state);
void parse_alarms(const Artifact& artifact, ImportState& state);
void parse_metadata(const Artifact& artifact, std::string_view count_key, ImportState& state,
                   std::initializer_list<std::string_view> required);

}  /* namespace context_hmi::context_import::detail */
