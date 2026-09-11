#pragma once

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace context_hmi {

using Json = nlohmann::json;

class DomainError final : public std::runtime_error {
 public:
  std::string code;
  std::string details;

  DomainError(std::string code, std::string details);
};

/** Validate declared identities, references and bounded fields; throw DomainError on failure. */
Json validate_model(const Json& model);
/** Resolve a task against this exact model identity/revision without inventing relationships. */
Json resolve_task(const Json& model, const Json& task);
/** Compare a trusted saved view with current declarations and retain unresolved conflicts. */
Json reconcile_view(const Json& model, const Json& previous_view);
}  /* namespace context_hmi */
