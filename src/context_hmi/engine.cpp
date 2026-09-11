#include "context_hmi/engine_internal.hpp"

#include <utility>

namespace context_hmi {

DomainError::DomainError(std::string code_value, std::string details_value)
    : std::runtime_error(code_value + ": " + details_value),
      code(std::move(code_value)),
      details(std::move(details_value)) {}

Json validate_model(const Json &model) {
    const internal::ModelIndex index = internal::index_model(model);
    return Json{{"valid", true},
                {"model_id", model.at("model_id")},
                {"revision", model.at("revision")},
                {"asset_count", index.assets.size()},
                {"relationship_count", index.relationships.size()},
                {"tag_count", index.tags.size()},
                {"alarm_count", index.alarms.size()}};
}

Json resolve_task(const Json &model, const Json &task) {
    const internal::ModelIndex index = internal::index_model(model);
    return internal::resolve_task_impl(index, task);
}

Json reconcile_view(const Json &model, const Json &previous_view) {
    const internal::ModelIndex index = internal::index_model(model);
    return internal::reconcile_view_impl(index, previous_view);
}

}  /* namespace context_hmi */
