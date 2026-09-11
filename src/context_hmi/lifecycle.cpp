#include "context_hmi/lifecycle.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace context_hmi::lifecycle {
namespace {

constexpr std::size_t kMaximumComponentBytes = 128;
constexpr std::size_t kMaximumReasonBytes = 1024;

bool valid_state(State state) {
  return state == State::kDisabled || state == State::kStarting || state == State::kReady ||
         state == State::kPaused || state == State::kDegraded || state == State::kStopping ||
         state == State::kStopped || state == State::kFailed;
}

bool allowed(State current, State next) {
  if (current == next) {
    return true;
  }
  switch (current) {
    case State::kDisabled:
      return next == State::kStarting || next == State::kStopping || next == State::kStopped;
    case State::kStarting:
      return next == State::kReady || next == State::kDegraded || next == State::kFailed ||
             next == State::kStopping;
    case State::kReady:
      return next == State::kPaused || next == State::kDegraded || next == State::kFailed ||
             next == State::kStopping;
    case State::kPaused:
      return next == State::kReady || next == State::kDegraded || next == State::kFailed ||
             next == State::kStopping;
    case State::kDegraded:
      return next == State::kReady || next == State::kPaused || next == State::kFailed ||
             next == State::kStopping;
    case State::kFailed:
      return next == State::kStarting || next == State::kStopping;
    case State::kStopping:
      return next == State::kStopped || next == State::kFailed;
    case State::kStopped:
      return false;
  }
  return false;
}

}  /* namespace */

Machine::Machine(std::string component, State initial)
    : component_(std::move(component)), state_(initial) {
  if (component_.empty() || component_.size() > kMaximumComponentBytes) {
    throw std::invalid_argument("lifecycle component must be 1 to 128 bytes");
  }
  if (!valid_state(initial)) {
    throw std::invalid_argument("lifecycle initial state is invalid");
  }
}

void Machine::transition(State next, std::string reason) {
  if (reason.size() > kMaximumReasonBytes) {
    throw std::invalid_argument("lifecycle transition reason exceeds 1024 bytes");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!allowed(state_, next)) {
    throw std::logic_error("illegal " + component_ + " lifecycle transition from " +
                           state_name(state_) + " to " + state_name(next));
  }
  if (state_ != next) {
    if (transitions_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("lifecycle transition counter exhausted");
    }
    ++transitions_;
  }
  state_ = next;
  reason_ = std::move(reason);
}

State Machine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

nlohmann::json Machine::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nlohmann::json{{"component", component_},
                        {"state", state_name(state_)},
                        {"reason", reason_},
                        {"transitions", transitions_}};
}

std::string state_name(State state) {
  switch (state) {
    case State::kDisabled:
      return "disabled";
    case State::kStarting:
      return "starting";
    case State::kReady:
      return "ready";
    case State::kPaused:
      return "paused";
    case State::kDegraded:
      return "degraded";
    case State::kStopping:
      return "stopping";
    case State::kStopped:
      return "stopped";
    case State::kFailed:
      return "failed";
  }
  throw std::logic_error("unknown lifecycle state");
}

}  /* namespace context_hmi::lifecycle */
