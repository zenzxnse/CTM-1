#include "context_hmi/telemetry_store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "context_hmi/engine.hpp"

namespace context_hmi::telemetry {
namespace {

constexpr std::size_t kMaximumSourceIdBytes = 128;
constexpr std::size_t kMaximumStringValueBytes = 2048;

bool valid_identity(const std::string& value) {
  if (value.empty() || value.size() > kMaximumSourceIdBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-' || character == '.';
  });
}

void require_fields(const Json& object,
                    std::initializer_list<const char*> allowed,
                    const std::string& location) {
  if (!object.is_object()) {
    throw DomainError("invalid_telemetry", location + " must be an object");
  }
  for (const auto& field : object.items()) {
    const bool found = std::any_of(
        allowed.begin(), allowed.end(),
        [&](const char* name) { return field.key() == name; });
    if (!found) {
      throw DomainError("invalid_telemetry",
                        location + " contains unsupported field '" +
                            field.key() + "'");
    }
  }
}

bool numeric_type(const std::string& type) {
  return type == "number" || type == "float" || type == "double" ||
         type == "integer";
}

void validate_value(const Json& sample, const Json& tag,
                    const std::string& location) {
  require_fields(sample, {"value", "quality", "timestamp_ms"}, location);
  if (!sample.contains("value") || !sample.contains("quality") ||
      !sample.contains("timestamp_ms")) {
    throw DomainError("invalid_telemetry",
                      location + " requires value, quality and timestamp_ms");
  }
  if (!sample.at("quality").is_string()) {
    throw DomainError("invalid_telemetry", location + ".quality must be a string");
  }
  static const std::set<std::string> qualities{"good", "uncertain", "bad",
                                               "unknown"};
  if (!qualities.contains(sample.at("quality").get<std::string>())) {
    throw DomainError("invalid_telemetry", location + ".quality is unsupported");
  }
  if (!sample.at("timestamp_ms").is_number_unsigned()) {
    throw DomainError("invalid_telemetry",
                      location + ".timestamp_ms must be an unsigned integer");
  }
  const auto type = tag.at("data_type").get<std::string>();
  const auto& value = sample.at("value");
  if (numeric_type(type)) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) ||
        (type == "integer" && !value.is_number_integer() &&
         !value.is_number_unsigned())) {
      throw DomainError("invalid_telemetry",
                        location + ".value does not match numeric tag type");
    }
  } else if (type == "boolean" || type == "bool") {
    if (!value.is_boolean()) {
      throw DomainError("invalid_telemetry",
                        location + ".value does not match boolean tag type");
    }
  } else if (!value.is_string() ||
             value.get_ref<const std::string&>().size() >
                 kMaximumStringValueBytes) {
    throw DomainError("invalid_telemetry",
                      location + ".value does not match bounded string tag type");
  }
}

bool compare(double value, const std::string& operation, double threshold) {
  if (operation == ">") {
    return value > threshold;
  }
  if (operation == ">=") {
    return value >= threshold;
  }
  if (operation == "<") {
    return value < threshold;
  }
  if (operation == "<=") {
    return value <= threshold;
  }
  if (operation == "==") {
    return value == threshold;
  }
  return value != threshold;
}

std::unordered_map<std::string, const Json*> tags_by_id(const Json& model) {
  std::unordered_map<std::string, const Json*> result;
  result.reserve(model.at("tags").size());
  for (const auto& tag : model.at("tags")) {
    result.emplace(tag.at("id").get<std::string>(), &tag);
  }
  return result;
}

}  /* namespace */

Store::Store(std::uint64_t stale_after_ms, std::size_t maximum_values_per_batch)
    : stale_after_ms_(stale_after_ms),
      maximum_values_per_batch_(maximum_values_per_batch) {
  if (stale_after_ms_ < 100 || stale_after_ms_ > 300000) {
    throw std::invalid_argument("telemetry stale interval is out of range");
  }
  if (maximum_values_per_batch_ == 0 || maximum_values_per_batch_ > 4096) {
    throw std::invalid_argument("telemetry value limit is out of range");
  }
}

std::uint64_t Store::unix_time_ms() {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return static_cast<std::uint64_t>(elapsed.count());
}

Json Store::ingest(const Json& model, std::uint64_t context_generation,
                   const Json& batch) {
  try {
    validate_model(model);
    require_fields(batch,
                   {"schema_version", "model_id", "model_revision",
                    "context_generation", "source_id", "sequence", "values"},
                   "telemetry");
    if (batch.size() != 7 ||
        batch.value("schema_version", "") != "context-hmi-telemetry/1" ||
        !batch.contains("model_id") || !batch.at("model_id").is_string() ||
        batch.at("model_id") != model.at("model_id") ||
        !batch.contains("model_revision") ||
        !batch.at("model_revision").is_number_unsigned() ||
        batch.at("model_revision") != model.at("revision") ||
        !batch.contains("context_generation") ||
        !batch.at("context_generation").is_number_unsigned() ||
        batch.at("context_generation").get<std::uint64_t>() !=
            context_generation) {
      throw DomainError("stale_context",
                        "telemetry identity, revision or generation is not current");
    }
    if (!batch.contains("source_id") || !batch.at("source_id").is_string() ||
        !valid_identity(batch.at("source_id").get<std::string>())) {
      throw DomainError("invalid_telemetry", "telemetry.source_id is invalid");
    }
    if (!batch.contains("sequence") ||
        !batch.at("sequence").is_number_unsigned()) {
      throw DomainError("invalid_telemetry",
                        "telemetry.sequence must be an unsigned integer");
    }
    if (!batch.contains("values") || !batch.at("values").is_object() ||
        batch.at("values").empty() ||
        batch.at("values").size() > maximum_values_per_batch_) {
      throw DomainError("invalid_telemetry",
                        "telemetry.values exceeds the configured bounded value count");
    }
    const auto tags = tags_by_id(model);
    for (const auto& sample : batch.at("values").items()) {
      const auto found = tags.find(sample.key());
      if (found == tags.end()) {
        throw DomainError("invalid_telemetry",
                          "telemetry value references undeclared tag '" +
                              sample.key() + "'");
      }
      validate_value(sample.value(), *found->second,
                     "telemetry.values." + sample.key());
    }
    const auto source_id = batch.at("source_id").get<std::string>();
    const auto sequence = batch.at("sequence").get<std::uint64_t>();
    const auto received = unix_time_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_generation_ != context_generation) {
      throw DomainError("stale_context",
                        "machine context changed before telemetry commit");
    }
    const auto previous = source_sequences_.find(source_id);
    if (previous != source_sequences_.end() && sequence <= previous->second) {
      throw DomainError("telemetry_out_of_order",
                        "telemetry sequence did not advance for this source");
    }
    for (const auto& sample : batch.at("values").items()) {
      Json current = sample.value();
      current["received_timestamp_ms"] = received;
      current["source_id"] = source_id;
      current["sequence"] = sequence;
      values_[sample.key()] = std::move(current);
    }
    source_sequences_[source_id] = sequence;
    ++accepted_batches_;
    accepted_values_ += batch.at("values").size();
    last_received_ms_ = received;
    return Json{{"accepted", true},
                {"source_id", source_id},
                {"sequence", sequence},
                {"value_count", batch.at("values").size()},
                {"context_generation", context_generation},
                {"received_timestamp_ms", received}};
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++rejected_batches_;
    throw;
  }
}

Json Store::snapshot(const Json& model, std::uint64_t context_generation,
                     const std::string& session_id) const {
  validate_model(model);
  const auto now = unix_time_ms();
  Json result{{"schema_version", "context-hmi-telemetry/1"},
              {"model_id", model.at("model_id")},
              {"model_revision", model.at("revision")},
              {"context_generation", context_generation},
              {"session_id", session_id},
              {"quality", "unknown"},
              {"values", Json::object()},
              {"alarms", Json::array()}};
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t good = 0;
  std::size_t stale = 0;
  std::size_t unavailable = 0;
  for (const auto& tag : model.at("tags")) {
    const auto id = tag.at("id").get<std::string>();
    const auto found = values_.find(id);
    if (context_generation_ != context_generation || found == values_.end()) {
      result["values"][id] =
          Json{{"value", nullptr},
               {"quality", "unknown"},
               {"unit", tag.at("unit")},
               {"timestamp_ms", nullptr},
               {"age_ms", nullptr}};
      ++unavailable;
      continue;
    }
    Json sample = *found;
    const auto received = sample.at("received_timestamp_ms").get<std::uint64_t>();
    const auto age = now - std::min(now, received);
    sample["age_ms"] = age;
    sample["unit"] = tag.at("unit");
    if (age > stale_after_ms_ && sample.value("quality", "unknown") == "good") {
      sample["quality"] = "stale";
    }
    if (sample.value("quality", "unknown") == "good") {
      ++good;
    } else if (sample.value("quality", "unknown") == "stale") {
      ++stale;
    } else {
      ++unavailable;
    }
    result["values"][id] = std::move(sample);
  }
  if (good > 0 && stale == 0 && unavailable == 0) {
    result["quality"] = "good";
  } else if (good > 0) {
    result["quality"] = "partial";
  } else if (stale > 0) {
    result["quality"] = "stale";
  }
  for (const auto& alarm : model.at("alarms")) {
    const auto tag_id = alarm.at("tag_id").get<std::string>();
    const auto& sample = result["values"].at(tag_id);
    const bool available = sample.at("value").is_number() &&
                           sample.value("quality", "unknown") != "bad" &&
                           sample.value("quality", "unknown") != "unknown";
    const double value = available ? sample.at("value").get<double>() : 0.0;
    result["alarms"].push_back(
        Json{{"id", alarm.at("id")},
             {"alarm_id", alarm.at("id")},
             {"active",
              available &&
                  compare(value, alarm.at("operator").get<std::string>(),
                          alarm.at("threshold").get<double>())},
             {"available", available},
             {"asset_id", alarm.at("asset_id")},
             {"tag_id", tag_id},
             {"name", alarm.at("name")},
             {"severity", alarm.at("severity")},
             {"value", available ? Json(value) : Json(nullptr)},
             {"threshold", alarm.at("threshold")}});
  }
  return result;
}

void Store::reconfigure(std::uint64_t context_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  values_.clear();
  source_sequences_.clear();
  context_generation_ = context_generation;
  last_received_ms_ = 0;
}

Json Store::readiness(std::uint64_t context_generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool current = context_generation_ == context_generation;
  const bool received = current && last_received_ms_ != 0;
  return Json{{"kind", "telemetry-ingestion"},
              {"ready", received},
              {"reason", received ? "current data received"
                                  : "waiting for current telemetry"},
              {"current_values", received ? values_.size() : 0}};
}

Json Store::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Json{{"kind", "telemetry-ingestion"},
              {"context_generation", context_generation_},
              {"current_values", values_.size()},
              {"sources", source_sequences_.size()},
              {"accepted_batches", accepted_batches_},
              {"rejected_batches", rejected_batches_},
              {"accepted_values", accepted_values_},
              {"last_received_timestamp_ms", last_received_ms_},
              {"stale_after_ms", stale_after_ms_},
              {"maximum_values_per_batch", maximum_values_per_batch_}};
}

}  /* namespace context_hmi::telemetry */
