#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "context_hmi/coroutine_task.hpp"

namespace context_hmi::execution {

class QueueFull : public std::runtime_error {
 public:
  QueueFull() : std::runtime_error("execution queue is full") {}
};

class WorkerPool {
 public:
  WorkerPool(std::size_t worker_count, std::size_t queue_capacity);
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  ~WorkerPool();

  template <typename Function>
  auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
    using Result = std::invoke_result_t<Function>;
    static_assert(!std::is_void_v<Result>, "worker jobs must return a result");
    static_assert(!std::is_reference_v<Result>, "worker jobs must return by value");
    auto task = std::make_shared<CoroutineTask<Result>>(
        invoke_coroutine(std::forward<Function>(function)));
    auto future = task->future();
    if (!enqueue([task]() mutable { task->resume(); })) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      throw QueueFull();
    }
    return future;
  }

  /** Enqueue a callback-completing job without creating a waiting future. */
  void dispatch(std::function<void()> job);

  void shutdown();
  nlohmann::json snapshot() const;

 private:
  bool enqueue(std::function<void()> job);
  void run();

  const std::size_t queue_capacity_;
  const std::size_t worker_count_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool accepting_{true};
  bool stopping_{false};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<std::uint64_t> active_{0};
};

}  /* namespace context_hmi::execution */
