#include "context_hmi/context_import_internal.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace context_hmi::context_import::detail {
namespace {

constexpr std::size_t kIdentityLimit = 256U;

}  /* namespace */

[[noreturn]] void fail(std::string code, const std::string& path, std::string message) {
  throw ImportError(std::move(code), path, std::move(message));
}

void require_string(const Json& object, std::string_view key, const std::string& path,
                    std::size_t limit, bool allow_empty) {
  if (!object.contains(key) || !object.at(key).is_string()) {
    fail("invalid_field", path, "required string field is missing");
  }
  const auto& value = object.at(key).get_ref<const std::string&>();
  if ((!allow_empty && value.empty()) || value.size() > std::min(limit, kIdentityLimit) ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) {
    fail("invalid_field", path, "string field is empty or exceeds its bound");
  }
}

std::string string_field(const Json& object, std::string_view key, const std::string& path,
                         std::size_t limit, bool allow_empty) {
  require_string(object, key, path, limit, allow_empty);
  return object.at(key).get<std::string>();
}

void allowed_keys(const Json& object, std::initializer_list<std::string_view> allowed,
                  const std::string& path) {
  const std::set<std::string_view> keys(allowed.begin(), allowed.end());
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
    if (current->is_string() &&
        current->get_ref<const std::string&>().size() > limits.max_string_bytes) {
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

bool valid_identity(std::string_view value) {
  if (value.empty() || value.size() > kIdentityLimit ||
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

void record_source(ImportState& state, const Artifact& artifact, std::size_t records) {
  state.total_records += records;
  if (state.total_records > state.limits->max_total_records) {
    fail("too_many_records", "manifest.artifacts", "bundle exceeds total record limit");
  }
  state.report.at("artifacts").push_back(source_record(artifact, records));
}

void sort_by_id(Json& array) {
  std::sort(array.begin(), array.end(), [](const Json& left, const Json& right) {
    return left.at("id").get<std::string>() < right.at("id").get<std::string>();
  });
}

}  /* namespace context_hmi::context_import::detail */
