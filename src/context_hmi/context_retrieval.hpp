#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace context_hmi::context {

using Json = nlohmann::json;

struct RetrievalLimits {
  std::size_t candidates{12};
  std::size_t assets{16};
  std::size_t relationships{64};
  std::size_t tags{128};
  std::size_t alarms{128};
  std::size_t max_serialized_bytes{12288};
  std::size_t max_estimated_tokens{3072};
};

/** Build a deterministic, bounded subgraph relevant to one operator request. */
Json retrieve(const Json& model, const std::string& prompt,
              const RetrievalLimits& limits = {});

/** Reject a schema-valid interpretation that conflicts with retrieval evidence. */
void validate_interpretation_scope(const Json& model, const Json& retrieval,
                                   const Json& interpretation);

}  /* namespace context_hmi::context */
