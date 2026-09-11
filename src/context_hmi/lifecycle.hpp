#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace context_hmi::lifecycle {

enum class State {
  kDisabled,
  kStarting,
  kReady,
  kPaused,
  kDegraded,
  kStopping,
  kStopped,
  kFailed,
};

/** Thread-safe finite-state machine with a fixed transition table. */
class Machine {
 public:
  Machine(std::string component, State initial);

  void transition(State next, std::string reason = {});
  State state() const;
  nlohmann::json snapshot() const;

 private:
  std::string component_;
  mutable std::mutex mutex_;
  State state_;
  std::string reason_;
  std::uint64_t transitions_{0};
};

std::string state_name(State state);

}  /* namespace context_hmi::lifecycle */
