/*
 * Lifecycle state-machine contract tests.
 *
 * Service, provider, telemetry and storage components use this same explicit transition
 * table. Illegal transitions must fail without changing state, including after shutdown.
 */

#include <stdexcept>
#include <string>

#include "context_hmi/lifecycle.hpp"
#include "support/check.hpp"

using context_hmi::lifecycle::Machine;
using context_hmi::lifecycle::State;
using context_hmi_test::check;

namespace {

void an_explicit_start_ready_pause_stop_path_is_recorded() {
    Machine machine("provider", State::kDisabled);
    machine.transition(State::kStarting, "configuration accepted");
    machine.transition(State::kReady, "provider reachable");
    machine.transition(State::kPaused, "operator paused inference");
    machine.transition(State::kReady, "operator resumed inference");
    machine.transition(State::kStopping, "service shutdown");
    machine.transition(State::kStopped, "workers joined");

    const auto snapshot = machine.snapshot();
    check(snapshot.at("component") == "provider", "snapshot identifies the component");
    check(snapshot.at("state") == "stopped", "snapshot reports the final state");
    check(snapshot.at("reason") == "workers joined", "snapshot reports the transition reason");
    check(snapshot.at("transitions") == 6, "only actual state changes count as transitions");
}

void repeated_state_is_idempotent_but_updates_reason() {
    Machine machine("telemetry", State::kStarting);
    machine.transition(State::kStarting, "still loading");
    check(machine.state() == State::kStarting, "repeating a state keeps the state");
    const auto snapshot = machine.snapshot();
    check(snapshot.at("transitions") == 0, "repeating a state is not a new transition");
    check(snapshot.at("reason") == "still loading", "the latest reason remains visible");
}

void illegal_transitions_are_atomic_and_repeatable() {
    Machine machine("store", State::kStopped);
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool rejected = false;
        try {
            machine.transition(State::kReady, "must fail");
        } catch (const std::logic_error &) {
            rejected = true;
        }
        check(rejected, "a stopped component cannot restart implicitly");
        check(machine.state() == State::kStopped, "an illegal transition does not mutate state");
    }
    check(machine.snapshot().at("transitions") == 0,
          "repeated illegal transitions do not alter accounting");
}

void failed_components_can_only_retry_through_starting() {
    Machine machine("inference", State::kStarting);
    machine.transition(State::kFailed, "timeout");
    bool rejected = false;
    try {
        machine.transition(State::kReady, "skip retry");
    } catch (const std::logic_error &) {
        rejected = true;
    }
    check(rejected, "failure cannot jump directly to ready");
    machine.transition(State::kStarting, "retry");
    machine.transition(State::kDegraded, "retry unavailable");
    check(machine.state() == State::kDegraded, "a retry can expose degraded availability");
}

}  /* namespace */

int main() {
    bool rejected = false;
    try {
        Machine invalid("", State::kDisabled);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "an unnamed lifecycle component is rejected");
    an_explicit_start_ready_pause_stop_path_is_recorded();
    repeated_state_is_idempotent_but_updates_reason();
    illegal_transitions_are_atomic_and_repeatable();
    failed_components_can_only_retry_through_starting();
    return context_hmi_test::report("lifecycle tests");
}
