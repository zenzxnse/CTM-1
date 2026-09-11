/*
 * Bounded cache contract tests.
 *
 * The cache is used for retrieved context and provider interpretations. A hit must return
 * an immutable value, entries must never exceed capacity, expiry must be observable, and
 * invalidation must remove every revision-specific value.
 */

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>

#include "context_hmi/bounded_cache.hpp"
#include "support/check.hpp"

using context_hmi::Json;
using context_hmi::cache::BoundedJsonCache;
using context_hmi::cache::Config;
using context_hmi_test::check;

namespace {

void invalid_bounds_are_rejected() {
    bool rejected = false;
    try {
        BoundedJsonCache cache(Config{0, std::chrono::seconds(1)});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "a zero-capacity cache is rejected");

    rejected = false;
    try {
        BoundedJsonCache cache(Config{1, std::chrono::milliseconds(999)});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "a sub-second cache lifetime is rejected");
}

void values_are_copied_and_hits_are_counted() {
    BoundedJsonCache cache(Config{2, std::chrono::seconds(5)});
    Json value{{"task", "overview"}, {"assets", Json::array({"line-1"})}};
    cache.put("model:1:overview", value);
    value["task"] = "tampered";

    auto found = cache.get("model:1:overview");
    check(found.has_value(), "a stored value can be read");
    if (found.has_value()) {
        check(found->at("task") == "overview", "the cache owns a value copy");
        (*found)["task"] = "caller mutation";
    }
    const auto second = cache.get("model:1:overview");
    check(second.has_value() && second->at("task") == "overview",
          "a caller cannot mutate the cached value");
    const Json metrics = cache.snapshot();
    check(metrics.at("hits") == 2, "cache hits are counted");
    check(metrics.at("misses") == 0, "cache misses remain zero for found keys");
}

void capacity_evicts_the_least_recently_used_entry() {
    BoundedJsonCache cache(Config{2, std::chrono::seconds(5)});
    cache.put("a", Json{{"value", 1}});
    cache.put("b", Json{{"value", 2}});
    check(cache.get("a").has_value(), "reading an entry refreshes its recency");
    cache.put("c", Json{{"value", 3}});

    check(cache.get("a").has_value(), "the most recently used entry is retained");
    check(!cache.get("b").has_value(), "the least recently used entry is evicted");
    check(cache.get("c").has_value(), "the replacement entry is retained");
    const Json metrics = cache.snapshot();
    check(metrics.at("entries") == 2, "entry count never exceeds capacity");
    check(metrics.at("evictions") == 1, "an eviction is observable");
}

void expiry_is_a_miss_and_does_not_return_stale_context() {
    BoundedJsonCache cache(Config{2, std::chrono::seconds(1)});
    cache.put("revision-7", Json{{"generation", 7}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(!cache.get("revision-7").has_value(), "an expired entry is not returned");
    const Json metrics = cache.snapshot();
    check(metrics.at("expirations") == 1, "expiry is counted separately from eviction");
    check(metrics.at("misses") == 1, "an expired lookup is a miss");
}

void clear_invalidates_revision_entries() {
    BoundedJsonCache cache(Config{3, std::chrono::seconds(5)});
    cache.put("model-a:revision-1:generation-4", Json{{"scope", "line-1"}});
    cache.put("model-a:revision-2:generation-5", Json{{"scope", "line-2"}});
    cache.clear();
    check(!cache.get("model-a:revision-1:generation-4").has_value(),
          "clearing removes the old revision");
    check(!cache.get("model-a:revision-2:generation-5").has_value(),
          "clearing removes the current revision");
    const Json metrics = cache.snapshot();
    check(metrics.at("entries") == 0, "clear leaves no entries");
    check(metrics.at("invalidations") == 2, "clear counts invalidated entries");
}

}  /* namespace */

int main() {
    invalid_bounds_are_rejected();
    values_are_copied_and_hits_are_counted();
    capacity_evicts_the_least_recently_used_entry();
    expiry_is_a_miss_and_does_not_return_stale_context();
    clear_invalidates_revision_entries();
    return context_hmi_test::report("cache tests");
}
