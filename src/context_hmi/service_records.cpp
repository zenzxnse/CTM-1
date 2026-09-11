#include "context_hmi/service_runtime.hpp"

#include <algorithm>
#include <string>

#include "context_hmi/engine.hpp"

namespace context_hmi::service {

void Runtime::append_operational(const std::string& kind, const Json& record) {
  if (journal_) {
    journal_->append(kind, record);
  }
}

void Runtime::restore_journal_records() {
  if (!journal_) {
    return;
  }
  for (const auto& record : journal_->recent(512)) {
    if (record.kind != "saved_task" || !record.payload.is_object() ||
        !record.payload.contains("task") ||
        !record.payload.at("task").is_object()) {
      continue;
    }
    try {
      Json task = record.payload.at("task");
      if (task.value("model_id", "") !=
          model_.at("model_id").get<std::string>()) {
        continue;
      }
      task["model_revision"] = model_.at("revision");
      auto view = resolve_task(model_, task);
      const auto id = session_id_ + "-restored-view-" +
                      std::to_string(record.sequence);
      view["view_id"] = id;
      view["session_id"] = session_id_;
      view["context_generation"] =
          model_generation_.load(std::memory_order_relaxed);
      if (views_.size() >= maximum_saved_views()) {
        views_.erase(view_order_.front());
        view_order_.pop_front();
      }
      views_[id] = std::move(view);
      view_order_.push_back(id);
    } catch (const std::exception& error) {
      diagnostic_logger_.write(
          diagnostics::Severity::kWarning, "saved_task_restore_rejected",
          Json{{"sequence", record.sequence}, {"reason", error.what()}});
    }
  }
}

Json Runtime::audit(std::size_t limit) const {
  Json operational = records(limit);
  operational["command_gateway"] = "disabled";
  operational["record_class"] = "durable-operational";
  return operational;
}

Json Runtime::records(std::size_t limit) const {
  if (!journal_) {
    return Json{{"records", Json::array()},
                {"store", Json{{"enabled", false}}}};
  }
  Json records = Json::array();
  for (const auto& record :
       journal_->recent(std::min<std::size_t>(limit, 100))) {
    records.push_back(Json{{"sequence", record.sequence},
                           {"kind", record.kind},
                           {"payload", record.payload}});
  }
  return Json{{"records", std::move(records)},
              {"store", journal_->snapshot()}};
}

}  /* namespace context_hmi::service */
