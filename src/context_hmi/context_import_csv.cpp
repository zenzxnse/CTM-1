#include "context_hmi/context_import_internal.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace context_hmi::context_import::detail {

Rows parse_csv(std::string_view contents, const std::string& path, const Limits& limits) {
  Rows rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  bool after_quote = false;
  bool saw_character = false;
  auto check_field = [&]() {
    if (field.size() > limits.max_field_bytes) {
      fail("field_too_large", path, "CSV field exceeds the configured byte limit");
    }
  };
  auto finish_row = [&]() {
    check_field();
    row.push_back(std::move(field));
    field.clear();
    if (row.size() > limits.max_columns) {
      fail("too_many_columns", path, "CSV row exceeds the configured column limit");
    }
    if (row.size() == 1U && row.front().empty() && !saw_character) {
      row.clear();
      return;
    }
    rows.push_back(std::move(row));
    row.clear();
    saw_character = false;
    if (rows.size() > limits.max_records_per_artifact + 1U) {
      fail("too_many_records", path, "CSV artifact exceeds the configured record limit");
    }
  };

  for (std::size_t index = 0; index < contents.size(); ++index) {
    const char character = contents[index];
    if (quoted) {
      if (character == '"') {
        if (index + 1U < contents.size() && contents[index + 1U] == '"') {
          field.push_back('"');
          ++index;
        } else {
          quoted = false;
          after_quote = true;
        }
      } else {
        field.push_back(character);
        saw_character = true;
        check_field();
      }
      continue;
    }
    if (after_quote) {
      if (character == ',') {
        check_field();
        row.push_back(std::move(field));
        field.clear();
        if (row.size() >= limits.max_columns) {
          fail("too_many_columns", path, "CSV row exceeds the configured column limit");
        }
        after_quote = false;
      } else if (character == '\r' || character == '\n') {
        after_quote = false;
        if (character == '\r' && index + 1U < contents.size() && contents[index + 1U] == '\n') {
          ++index;
        }
        finish_row();
      } else if (character == ' ' || character == '\t') {
        fail("invalid_csv", path, "whitespace after a quoted field is not accepted");
      } else {
        fail("invalid_csv", path, "characters after a quoted field are not accepted");
      }
      continue;
    }
    if (character == '"') {
      if (!field.empty()) {
        fail("invalid_csv", path, "a quote may start only an empty field");
      }
      quoted = true;
      saw_character = true;
    } else if (character == ',') {
      check_field();
      row.push_back(std::move(field));
      field.clear();
      if (row.size() >= limits.max_columns) {
        fail("too_many_columns", path, "CSV row exceeds the configured column limit");
      }
    } else if (character == '\r' || character == '\n') {
      if (character == '\r' && index + 1U < contents.size() && contents[index + 1U] == '\n') {
        ++index;
      }
      finish_row();
    } else {
      field.push_back(character);
      saw_character = true;
      check_field();
    }
  }
  if (quoted) {
    fail("invalid_csv", path, "quoted field is not closed");
  }
  if (after_quote || !field.empty() || !row.empty()) {
    finish_row();
  }
  if (rows.empty()) {
    fail("invalid_csv", path, "CSV artifact has no header row");
  }
  return rows;
}

std::map<std::string, std::size_t> header_map(const Rows& rows, const std::string& path,
                                               const Limits& limits) {
  std::map<std::string, std::size_t> headers;
  for (std::size_t index = 0; index < rows.front().size(); ++index) {
    const auto header = trim(rows.front()[index]);
    if (header.empty() || header.size() > limits.max_string_bytes ||
        !headers.emplace(header, index).second) {
      fail("invalid_csv", path, "CSV header is empty, oversized or duplicated");
    }
  }
  return headers;
}

std::string cell(const std::vector<std::string>& row,
                 const std::map<std::string, std::size_t>& headers, std::string_view key,
                 const std::string& path, const Limits& limits, bool required) {
  const auto found = headers.find(std::string(key));
  if (found == headers.end()) {
    if (required) {
      fail("invalid_csv", path, "required column is missing: " + std::string(key));
    }
    return {};
  }
  if (found->second >= row.size()) {
    if (required) {
      fail("invalid_csv", path, "row has fewer cells than its header");
    }
    return {};
  }
  if (row[found->second].size() > std::min(limits.max_field_bytes, std::size_t{256})) {
    fail("field_too_large", path, "CSV field exceeds the configured byte limit");
  }
  if (required && row[found->second].empty()) {
    fail("invalid_field", path, "required CSV field is empty");
  }
  if (std::any_of(row[found->second].begin(), row[found->second].end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) {
    fail("invalid_field", path, "CSV field contains a control character");
  }
  return row[found->second];
}

void require_columns(const std::map<std::string, std::size_t>& headers,
                     std::initializer_list<std::string_view> required, const std::string& path) {
  for (const auto name : required) {
    if (!headers.contains(std::string(name))) {
      fail("invalid_csv", path, "required column is missing: " + std::string(name));
    }
  }
}

void check_row_width(const Rows& rows, const std::string& path, const Limits& limits) {
  for (std::size_t row = 1U; row < rows.size(); ++row) {
    if (rows[row].size() != rows.front().size()) {
      fail("invalid_csv", path, "row has a different number of cells than its header");
    }
  }
  if (rows.size() - 1U > limits.max_records_per_artifact) {
    fail("too_many_records", path, "CSV artifact exceeds the configured record limit");
  }
}

}  /* namespace context_hmi::context_import::detail */
