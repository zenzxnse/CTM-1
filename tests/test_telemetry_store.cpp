/*
 * External telemetry-ingestion contract tests.
 *
 * The source adapter supplies a complete bounded batch. The store checks model identity,
 * revision, generation, declared tag ownership and value types before one atomic commit.
 */

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "context_hmi/telemetry_store.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

using context_hmi::DomainError;
using context_hmi::Json;
using context_hmi::telemetry::Store;
using context_hmi_test::base_model;
using context_hmi_test::check;

namespace {

Json sample_for(Json value) {
    return Json{{"value", std::move(value)},
                {"quality", "good"},
                {"timestamp_ms", static_cast<std::uint64_t>(1000)}};
}

Json complete_batch(const Json &model, std::uint64_t generation, const char *source,
                    std::uint64_t sequence) {
    Json values = Json::object();
    for (const auto &tag : model.at("tags")) {
        const auto type = tag.at("data_type").get<std::string>();
        Json value = type == "string" ? Json("running")
                                       : type == "boolean" ? Json(true) : Json(42.0);
        if (type == "integer")
            value = 42;
        values[tag.at("id")] = sample_for(std::move(value));
    }
    return Json{{"schema_version", "context-hmi-telemetry/1"},
                {"model_id", model.at("model_id")},
                {"model_revision",
                 static_cast<std::uint64_t>(model.at("revision").get<std::int64_t>())},
                {"context_generation", generation},
                {"source_id", source},
                {"sequence", sequence},
                {"values", std::move(values)}};
}

void an_empty_store_reports_unknown_values_and_not_ready() {
    const Json model = base_model();
    Store store(1000);
    store.reconfigure(7);
    const Json snapshot = store.snapshot(model, 7, "session-a");
    check(snapshot.at("quality") == "unknown", "no ingested data is unknown");
    check(snapshot.at("session_id") == "session-a", "snapshot carries the session identity");
    for (const auto &item : snapshot.at("values").items())
        check(item.value().at("quality") == "unknown", "missing tags remain unknown");
    check(!store.readiness(7).at("ready").get<bool>(), "readiness waits for current telemetry");
}

void a_valid_batch_is_committed_atomically_with_quality_and_provenance() {
    const Json model = base_model();
    Store store(1000);
    store.reconfigure(7);
    const Json accepted = store.ingest(model, 7, complete_batch(model, 7, "source-a", 1));
    check(accepted.at("accepted").get<bool>(), "a valid batch is accepted");
    check(accepted.at("value_count") == model.at("tags").size(),
          "the response reports the committed value count");

    const Json snapshot = store.snapshot(model, 7, "session-a");
    check(snapshot.at("quality") == "good", "all good values produce good overall quality");
    for (const auto &item : snapshot.at("values").items()) {
        check(item.value().at("quality") == "good", "committed values preserve quality");
        check(item.value().at("source_id") == "source-a", "source provenance is retained");
        check(item.value().at("sequence") == 1, "source sequence is retained");
        check(item.value().contains("received_timestamp_ms"),
              "receive time is recorded separately from source time");
    }
    check(store.readiness(7).at("ready").get<bool>(), "current data makes telemetry ready");
}

void identity_revision_and_sequence_boundaries_are_enforced() {
    const Json model = base_model();
    Store store(1000);
    store.reconfigure(7);
    const Json first = complete_batch(model, 7, "source-a", 3);
    (void)store.ingest(model, 7, first);

    Json wrong_generation = first;
    wrong_generation["context_generation"] = 8;
    try {
        (void)store.ingest(model, 7, wrong_generation);
        check(false, "a stale generation is rejected");
    } catch (const DomainError &error) {
        check(error.code == "stale_context", "stale generation has a named error");
    }

    Json duplicate = first;
    try {
        (void)store.ingest(model, 7, duplicate);
        check(false, "a duplicate source sequence is rejected");
    } catch (const DomainError &error) {
        check(error.code == "telemetry_out_of_order", "sequence refusal has a named error");
    }

    Json foreign_model = first;
    foreign_model["model_id"] = "different-model";
    try {
        (void)store.ingest(model, 7, foreign_model);
        check(false, "a foreign model identity is rejected");
    } catch (const DomainError &error) {
        check(error.code == "stale_context", "foreign identity has a stale-context error");
    }
}

void malformed_values_do_not_partially_commit() {
    const Json model = base_model();
    Store store(1000);
    store.reconfigure(7);
    Json invalid = complete_batch(model, 7, "source-a", 1);
    invalid["values"]["invented.tag"] = sample_for(7.0);
    try {
        (void)store.ingest(model, 7, invalid);
        check(false, "a batch containing an undeclared tag is rejected");
    } catch (const DomainError &error) {
        check(error.code == "invalid_telemetry", "undeclared tag has a validation error");
    }
    const Json snapshot = store.snapshot(model, 7, "session-a");
    for (const auto &item : snapshot.at("values").items())
        check(item.value().at("quality") == "unknown", "invalid batches commit no partial values");
    check(store.metrics().at("accepted_batches") == 0, "invalid batches are not accepted");
    check(store.metrics().at("rejected_batches") == 1, "invalid batches are counted");
}

void a_reconfiguration_clears_old_data_and_sequence_state() {
    const Json model = base_model();
    Store store(1000);
    store.reconfigure(7);
    (void)store.ingest(model, 7, complete_batch(model, 7, "source-a", 9));
    store.reconfigure(8);
    const Json cleared = store.snapshot(model, 8, "session-a");
    check(cleared.at("quality") == "unknown", "reconfiguration clears prior values");
    check(!store.readiness(8).at("ready").get<bool>(), "reconfiguration requires fresh data");

    const Json accepted = store.ingest(model, 8, complete_batch(model, 8, "source-a", 1));
    check(accepted.at("accepted").get<bool>(),
          "a source may restart sequence after a model generation change");
}

void stale_good_values_are_reported_as_stale_not_good() {
    const Json model = base_model();
    Store store(100);
    store.reconfigure(7);
    (void)store.ingest(model, 7, complete_batch(model, 7, "source-a", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(130));
    const Json snapshot = store.snapshot(model, 7, "session-a");
    check(snapshot.at("quality") == "stale", "aged values become stale");
    check(snapshot.at("values").at("tank-a.level").at("quality") == "stale",
          "each aged value exposes stale quality");
}

}  /* namespace */

int main() {
    an_empty_store_reports_unknown_values_and_not_ready();
    a_valid_batch_is_committed_atomically_with_quality_and_provenance();
    identity_revision_and_sequence_boundaries_are_enforced();
    malformed_values_do_not_partially_commit();
    a_reconfiguration_clears_old_data_and_sequence_state();
    stale_good_values_are_reported_as_stale_not_good();
    return context_hmi_test::report("telemetry store tests");
}
