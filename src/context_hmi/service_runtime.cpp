#include "context_hmi/service_runtime.hpp"

namespace context_hmi::service {
namespace {

#if defined(CONTEXT_HMI_CONSTRAINED)
constexpr std::size_t kMaximumSseClients = 2;
#else
constexpr std::size_t kMaximumSseClients = 3;
#endif
constexpr unsigned kMaximumInferenceRequests = 4;

}  /* namespace */

bool Runtime::acquire_sse_client() {
  unsigned clients = sse_clients_.load(std::memory_order_relaxed);
  while (clients < maximum_sse_clients()) {
    if (sse_clients_.compare_exchange_weak(clients, clients + 1,
                                           std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

void Runtime::release_sse_client() {
  unsigned current = sse_clients_.load(std::memory_order_relaxed);
  while (current != 0 &&
         !sse_clients_.compare_exchange_weak(current, current - 1,
                                             std::memory_order_acq_rel)) {
  }
}

std::size_t Runtime::maximum_sse_clients() const { return kMaximumSseClients; }

bool Runtime::acquire_inference() {
  unsigned current = inference_inflight_.load(std::memory_order_relaxed);
  while (current < kMaximumInferenceRequests) {
    if (inference_inflight_.compare_exchange_weak(current, current + 1,
                                                  std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

void Runtime::release_inference() {
  unsigned current = inference_inflight_.load(std::memory_order_relaxed);
  while (current != 0 &&
         !inference_inflight_.compare_exchange_weak(current, current - 1,
                                                    std::memory_order_acq_rel)) {
  }
}

}  /* namespace context_hmi::service */
