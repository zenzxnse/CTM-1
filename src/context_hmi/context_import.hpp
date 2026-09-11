#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace context_hmi::context_import {

using Json = nlohmann::json;

class ImportError final : public std::runtime_error {
 public:
  ImportError(std::string code, std::string path, std::string message);

  std::string code;
  std::string path;
};

/** Bounds all files, records and fields accepted from an engineering bundle. */
struct Limits {
  std::uintmax_t max_file_bytes{1024U * 1024U};
  std::size_t max_artifacts{32U};
  std::size_t max_records_per_artifact{8192U};
  std::size_t max_columns{32U};
  std::size_t max_field_bytes{4096U};
  std::size_t max_total_records{32768U};
  std::size_t max_string_bytes{4096U};
  std::size_t max_json_depth{32U};
  std::size_t max_json_nodes{32768U};
};

/** Options for importing one explicit bundle directory. */
struct Options {
  Limits limits{};
};

struct Result {
  Json model;
  Json report;
};

/**
 * Import a vendor-neutral engineering bundle whose manifest is at bundle/manifest.json.
 * The bundle is read-only. The returned model contains only the native machine-model fields.
 */
Result import_bundle(const std::filesystem::path& bundle, const Options& options = {});

/**
 * Import a manifest at an explicit path. Relative artifact paths are resolved below its
 * containing directory and cannot escape that directory.
 */
Result import_manifest(const std::filesystem::path& manifest, const Options& options = {});

}  /* namespace context_hmi::context_import */
