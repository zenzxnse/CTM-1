#include "context_hmi/context_retrieval.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "context_hmi/engine.hpp"

namespace context_hmi::context {
namespace {

using Terms = std::vector<std::string>;

constexpr std::size_t kMaxPrompt = 8192;

bool ascii_alphanumeric(unsigned char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9');
}

std::string canonical_term(std::string term) {
  static const std::unordered_map<std::string, std::string> synonyms{
      {"one", "1"},          {"first", "1"},       {"two", "2"},
      {"second", "2"},       {"three", "3"},       {"third", "3"},
      {"four", "4"},         {"fourth", "4"},      {"five", "5"},
      {"fifth", "5"},        {"warning", "alarm"}, {"warnings", "alarm"},
      {"alert", "alarm"},    {"alerts", "alarm"},  {"trips", "alarm"},
      {"reading", "overview"}, {"readings", "overview"},
      {"measurements", "overview"}, {"dashboard", "overview"},
      {"display", "overview"}, {"show", "overview"},
      {"monitor", "overview"}, {"inspect", "overview"},
      {"summary", "overview"}, {"temp", "temperature"}};
  const auto found = synonyms.find(term);
  return found == synonyms.end() ? term : found->second;
}

Terms terms_for(std::string_view text) {
  Terms terms;
  std::string current;
  for (const unsigned char character : text) {
    if (ascii_alphanumeric(character)) {
      const char lowered = character >= 'A' && character <= 'Z'
                               ? static_cast<char>(character - 'A' + 'a')
                               : static_cast<char>(character);
      current.push_back(lowered);
    } else if (!current.empty()) {
      terms.push_back(canonical_term(std::move(current)));
      current.clear();
    }
  }
  if (!current.empty()) {
    terms.push_back(canonical_term(std::move(current)));
  }
  std::sort(terms.begin(), terms.end());
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  return terms;
}

bool contains(const Terms& terms, const std::string& term) {
  return std::binary_search(terms.begin(), terms.end(), term);
}

bool has_scope_phrase(std::string_view prompt) {
  std::string normalized;
  normalized.reserve(prompt.size() + 2U);
  normalized.push_back(' ');
  for (const unsigned char character : prompt) {
    normalized.push_back(ascii_alphanumeric(character)
                             ? static_cast<char>(character >= 'A' && character <= 'Z'
                                                     ? character - 'A' + 'a'
                                                     : character)
                             : ' ');
  }
  normalized.push_back(' ');
  return normalized.find(" for ") != std::string::npos ||
         normalized.find(" on ") != std::string::npos ||
         normalized.find(" from ") != std::string::npos ||
         normalized.find(" at ") != std::string::npos;
}

std::set<std::string> numeric_terms(const Terms& terms) {
  std::set<std::string> result;
  for (const auto& term : terms) {
    if (!term.empty() &&
        std::all_of(term.begin(), term.end(), [](unsigned char character) {
          return character >= '0' && character <= '9';
        })) {
      result.insert(term);
    }
  }
  return result;
}

Terms asset_terms(const Json& asset) {
  Terms result;
  auto add = [&](const std::string& text) {
    auto terms = terms_for(text);
    result.insert(result.end(), terms.begin(), terms.end());
  };
  add(asset.at("id").get<std::string>());
  add(asset.at("name").get<std::string>());
  add(asset.at("kind").get<std::string>());
  if (asset.contains("aliases")) {
    for (const auto& alias : asset.at("aliases")) {
      add(alias.get<std::string>());
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

struct Candidate {
  const Json* asset;
  int score;
  Json reasons;
};

std::vector<std::string> task_hints(const Terms& prompt_terms) {
  std::vector<std::string> result;
  if (contains(prompt_terms, "alarm")) {
    result.push_back("alarms");
  } else if (contains(prompt_terms, "overview") ||
             contains(prompt_terms, "status") ||
             contains(prompt_terms, "state")) {
    result.push_back("overview");
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::string> measurement_role_hints(const Json& tags,
                                                const Terms& prompt_terms) {
  std::set<std::string> roles;
  for (const auto& tag : tags) {
    const auto role = tag.at("role").get<std::string>();
    auto role_terms = terms_for(role);
    const auto name_terms = terms_for(tag.at("name").get<std::string>());
    const auto all_present = [&](const Terms& terms) {
      return !terms.empty() &&
             std::all_of(terms.begin(), terms.end(), [&](const std::string& term) {
               return contains(prompt_terms, term);
             });
    };
    const bool matched = all_present(role_terms) || all_present(name_terms);
    if (matched) {
      roles.insert(role);
    }
  }
  return {roles.begin(), roles.end()};
}

bool model_has_asset(const Json& model, const std::string& id) {
  return std::any_of(model.at("assets").begin(), model.at("assets").end(),
                     [&](const Json& asset) { return asset.at("id") == id; });
}

}  /* namespace */

Json retrieve(const Json& model, const std::string& prompt, const RetrievalLimits& limits) {
  validate_model(model);
  if (prompt.empty() || prompt.size() > kMaxPrompt) {
    throw DomainError("invalid_request", "retrieval prompt must be nonempty and bounded");
  }
  if (limits.candidates == 0 || limits.assets == 0 || limits.relationships == 0 ||
      limits.tags == 0 || limits.alarms == 0) {
    throw DomainError("invalid_request", "retrieval limits must be positive");
  }

  const Terms prompt_terms = terms_for(prompt);
  const auto prompt_numbers = numeric_terms(prompt_terms);
  std::unordered_set<std::string> declared_kinds;
  for (const auto& asset : model.at("assets")) {
    const auto kind_terms = terms_for(asset.at("kind").get<std::string>());
    declared_kinds.insert(kind_terms.begin(), kind_terms.end());
  }
  const bool kind_mentioned = std::any_of(
      prompt_terms.begin(), prompt_terms.end(),
      [&](const std::string& term) { return declared_kinds.contains(term); });

  std::vector<Candidate> scored;
  for (const auto& asset : model.at("assets")) {
    const Terms terms = asset_terms(asset);
    const auto asset_numbers = numeric_terms(terms);
    const bool number_conflict = !prompt_numbers.empty() &&
                                 std::none_of(prompt_numbers.begin(), prompt_numbers.end(),
                                              [&](const std::string& number) {
                                                return asset_numbers.contains(number);
                                              });
    int score = 0;
    Json reasons = Json::array();
    if (!number_conflict) {
      for (const auto& term : terms) {
        if (contains(prompt_terms, term)) {
          score += term.size() > 2U ? 12 : 4;
          reasons.push_back("term:" + term);
        }
      }
      const auto id_terms = terms_for(asset.at("id").get<std::string>());
      if (!id_terms.empty() &&
          std::all_of(id_terms.begin(), id_terms.end(),
                      [&](const std::string& term) { return contains(prompt_terms, term); })) {
        score += 80;
        reasons.push_back("declared-id");
      }
      const auto name_terms = terms_for(asset.at("name").get<std::string>());
      if (!name_terms.empty() &&
          std::all_of(name_terms.begin(), name_terms.end(),
                      [&](const std::string& term) { return contains(prompt_terms, term); })) {
        score += 100;
        reasons.push_back("declared-name");
      }
      if (asset.contains("aliases")) {
        for (const auto& alias : asset.at("aliases")) {
          const auto alias_terms = terms_for(alias.get<std::string>());
          if (!alias_terms.empty() &&
              std::all_of(alias_terms.begin(), alias_terms.end(), [&](const std::string& term) {
                return contains(prompt_terms, term);
              })) {
            score += 90;
            reasons.push_back("declared-alias");
            break;
          }
        }
      }
    }
    if (score > 0) {
      scored.push_back(Candidate{&asset, score, std::move(reasons)});
    }
  }
  std::sort(scored.begin(), scored.end(), [](const Candidate& left, const Candidate& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    return left.asset->at("id").get<std::string>() <
           right.asset->at("id").get<std::string>();
  });

  const bool specific_reference = kind_mentioned || !prompt_numbers.empty() ||
                                  (scored.empty() && has_scope_phrase(prompt)) ||
                                  std::any_of(scored.begin(), scored.end(),
                                              [](const Candidate& item) {
                                                return item.score >= 80;
                                              });
  int eligibility_score = scored.empty() ? 0 : scored.front().score;
  if (eligibility_score < 12) {
    eligibility_score = 0;
  }

  Json candidates = Json::array();
  Json eligible = Json::array();
  std::unordered_set<std::string> scope_ids;
  for (const auto& item : scored) {
    if (candidates.size() >= limits.candidates) {
      break;
    }
    const auto id = item.asset->at("id").get<std::string>();
    const bool is_eligible = eligibility_score > 0 && item.score == eligibility_score;
    candidates.push_back(Json{{"asset_id", id},
                              {"name", item.asset->at("name")},
                              {"kind", item.asset->at("kind")},
                              {"score", item.score},
                              {"eligible", is_eligible},
                              {"reasons", item.reasons}});
    if (is_eligible) {
      eligible.push_back(id);
      scope_ids.insert(id);
    }
  }

  bool scope_truncated = false;
  if (scope_ids.empty() && !specific_reference) {
    for (const auto& asset : model.at("assets")) {
      if (scope_ids.size() >= limits.assets) {
        scope_truncated = true;
        break;
      }
      scope_ids.insert(asset.at("id").get<std::string>());
    }
  }
  if (scope_ids.size() == 1U) {
    for (const auto& relationship : model.at("relationships")) {
      const auto from = relationship.at("from").get<std::string>();
      const auto to = relationship.at("to").get<std::string>();
      if (scope_ids.contains(from) && scope_ids.size() < limits.assets) {
        scope_ids.insert(to);
      } else if (scope_ids.contains(to) && scope_ids.size() < limits.assets) {
        scope_ids.insert(from);
      } else if ((scope_ids.contains(from) && !scope_ids.contains(to)) ||
                 (scope_ids.contains(to) && !scope_ids.contains(from))) {
        scope_truncated = true;
      }
    }
  }

  Json assets = Json::array();
  for (const auto& asset : model.at("assets")) {
    if (!scope_ids.contains(asset.at("id").get<std::string>()) ||
        assets.size() >= limits.assets) {
      continue;
    }
    Json compact{{"id", asset.at("id")},
                 {"name", asset.at("name")},
                 {"kind", asset.at("kind")}};
    if (asset.contains("aliases")) {
      compact["aliases"] = asset.at("aliases");
    }
    assets.push_back(std::move(compact));
  }

  Json relationships = Json::array();
  std::size_t relevant_relationships = 0;
  for (const auto& relationship : model.at("relationships")) {
    const auto from = relationship.at("from").get<std::string>();
    const auto to = relationship.at("to").get<std::string>();
    if (scope_ids.contains(from) && scope_ids.contains(to)) {
      ++relevant_relationships;
      if (relationships.size() < limits.relationships) {
        relationships.push_back(Json{{"id", relationship.at("id")},
                                     {"from", relationship.at("from")},
                                     {"to", relationship.at("to")},
                                     {"kind", relationship.at("kind")}});
      }
    }
  }

  std::unordered_set<std::string> measurement_scope_ids = scope_ids;
  if (!eligible.empty()) {
    measurement_scope_ids.clear();
    for (const auto& id : eligible) {
      measurement_scope_ids.insert(id.get<std::string>());
    }
  }
  Json tags = Json::array();
  std::size_t relevant_tags = 0;
  for (const auto& tag : model.at("tags")) {
    if (measurement_scope_ids.contains(tag.at("asset_id").get<std::string>())) {
      ++relevant_tags;
      if (tags.size() < limits.tags) {
        tags.push_back(Json{{"id", tag.at("id")},
                            {"asset_id", tag.at("asset_id")},
                            {"name", tag.at("name")},
                            {"role", tag.at("role")},
                            {"data_type", tag.at("data_type")},
                            {"unit", tag.at("unit")}});
      }
    }
  }

  Json alarms = Json::array();
  std::size_t relevant_alarms = 0;
  for (const auto& alarm : model.at("alarms")) {
    if (measurement_scope_ids.contains(alarm.at("asset_id").get<std::string>())) {
      ++relevant_alarms;
      if (alarms.size() < limits.alarms) {
        alarms.push_back(Json{{"id", alarm.at("id")},
                              {"asset_id", alarm.at("asset_id")},
                              {"name", alarm.at("name")},
                              {"tag_id", alarm.at("tag_id")},
                              {"severity", alarm.at("severity")}});
      }
    }
  }

  auto hints = task_hints(prompt_terms);
  const auto role_hints = measurement_role_hints(tags, prompt_terms);
  if (hints.empty() && !role_hints.empty()) {
    hints.push_back("overview");
  }
  const bool ambiguous = eligible.size() > 1U;
  const bool unresolved_reference = specific_reference && eligible.empty();
  return Json{{"schema_version", "context-retrieval/1"},
              {"model_id", model.at("model_id")},
              {"model_revision", model.at("revision")},
              {"prompt_terms", prompt_terms},
              {"task_hints", hints},
              {"measurement_role_hints", role_hints},
              {"specific_asset_reference", specific_reference},
              {"requires_clarification", ambiguous || unresolved_reference},
              {"eligible_asset_ids", eligible},
              {"candidates", candidates},
              {"scope", Json{{"assets", assets},
                              {"relationships", relationships},
                              {"tags", tags},
                              {"alarms", alarms}}},
              {"limits", Json{{"candidate_limit", limits.candidates},
                              {"asset_limit", limits.assets},
                              {"relationship_limit", limits.relationships},
                              {"tag_limit", limits.tags},
                              {"alarm_limit", limits.alarms}}},
              {"truncated", Json{{"candidates", scored.size() > candidates.size()},
                                 {"assets", scope_truncated || scope_ids.size() > assets.size()},
                                 {"relationships", relevant_relationships > relationships.size()},
                                 {"tags", relevant_tags > tags.size()},
                                 {"alarms", relevant_alarms > alarms.size()}}}};
}

void validate_interpretation_scope(const Json& model, const Json& retrieval,
                                   const Json& interpretation) {
  if (interpretation.value("status", "") != "ready") {
    return;
  }
  const auto& task = interpretation.at("task");
  const auto hints = retrieval.value("task_hints", Json::array());
  if (hints.is_array() && hints.size() == 1U && task.at("kind") != hints.at(0)) {
    throw DomainError("interpretation_mismatch",
                      "provider task kind conflicts with retrieved request evidence");
  }
  if (retrieval.value("requires_clarification", false)) {
    throw DomainError("interpretation_mismatch",
                      "provider answered ready when retrieved context requires clarification");
  }
  if (!task.contains("anchor_asset_id")) {
    if (retrieval.value("specific_asset_reference", false)) {
      throw DomainError("interpretation_mismatch",
                        "provider omitted the asset identified by the operator request");
    }
  } else {
    const auto id = task.at("anchor_asset_id").get<std::string>();
    if (model_has_asset(model, id)) {
      const auto eligible = retrieval.value("eligible_asset_ids", Json::array());
      const bool matched = eligible.is_array() &&
                           std::find(eligible.begin(), eligible.end(), Json(id)) != eligible.end();
      if (!matched) {
        throw DomainError("interpretation_mismatch",
                          "provider selected an asset outside the retrieved operator scope");
      }
    }
  }
  if (!task.contains("measurement_roles")) {
    if (!retrieval.value("measurement_role_hints", Json::array()).empty()) {
      throw DomainError("interpretation_mismatch",
                        "provider omitted measurements identified by the operator request");
    }
    return;
  }
  std::set<std::string> available;
  for (const auto& tag : retrieval.at("scope").at("tags")) {
    available.insert(tag.at("role").get<std::string>());
  }
  std::set<std::string> selected;
  for (const auto& role : task.at("measurement_roles")) {
    const auto value = role.get<std::string>();
    if (!available.contains(value)) {
      throw DomainError("interpretation_mismatch",
                        "provider selected a measurement outside the retrieved operator scope");
    }
    selected.insert(value);
  }
  for (const auto& role : retrieval.value("measurement_role_hints", Json::array())) {
    if (!selected.contains(role.get<std::string>())) {
      throw DomainError("interpretation_mismatch",
                        "provider omitted a requested measurement role");
    }
  }
}

}  /* namespace context_hmi::context */
