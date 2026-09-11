#include "context_hmi/worker_pool.hpp"

#include <algorithm>

namespace context_hmi::execution {

WorkerPool::WorkerPool(std::size_t worker_count, std::size_t queue_capacity)
    : queue_capacity_(queue_capacity), worker_count_(worker_count) {
  constexpr std::size_t kMaximumWorkers = 256;
  if (worker_count == 0 || worker_count > kMaximumWorkers || queue_capacity == 0) {
    throw std::invalid_argument(
        "worker count must be in [1, 256] and queue capacity must be positive");
  }
  workers_.reserve(worker_count);
  try {
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this] { run(); });
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_ = false;
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    throw;
  }
}

WorkerPool::~WorkerPool() { shutdown(); }

void WorkerPool::dispatch(std::function<void()> job) {
  if (!job) {
    throw std::invalid_argument("worker job must not be empty");
  }
  if (!enqueue(std::move(job))) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    throw QueueFull();
  }
}

bool WorkerPool::enqueue(std::function<void()> job) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!accepting_ || queue_.size() >= queue_capacity_) {
    return false;
  }
  queue_.push_back(std::move(job));
  submitted_.fetch_add(1, std::memory_order_relaxed);
  condition_.notify_one();
  return true;
}

void WorkerPool::run() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      active_.fetch_add(1, std::memory_order_relaxed);
    }
    try {
      job();
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      /** Detached jobs own their error boundary; keep the pool alive on a defect. */
    }
    active_.fetch_sub(1, std::memory_order_relaxed);
    completed_.fetch_add(1, std::memory_order_relaxed);
  }
}

void WorkerPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    accepting_ = false;
    stopping_ = true;
  }
  condition_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

nlohmann::json WorkerPool::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nlohmann::json{{"workers", worker_count_},
                        {"queue_depth", queue_.size()},
                        {"queue_capacity", queue_capacity_},
                        {"accepting", accepting_},
                        {"active", active_.load(std::memory_order_relaxed)},
                        {"submitted", submitted_.load(std::memory_order_relaxed)},
                        {"completed", completed_.load(std::memory_order_relaxed)},
                        {"rejected", rejected_.load(std::memory_order_relaxed)},
                        {"failed", failed_.load(std::memory_order_relaxed)}};
}

}  /* namespace context_hmi::execution */
