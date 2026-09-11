#pragma once

#include <coroutine>
#include <exception>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace context_hmi::execution {

template <typename Result>
class CoroutineTask {
 public:
  static_assert(!std::is_void_v<Result>, "coroutine tasks must return a value");
  static_assert(!std::is_reference_v<Result>, "coroutine tasks must return by value");

  struct promise_type {
    std::promise<Result> completion;

    CoroutineTask get_return_object() {
      return CoroutineTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_value(Result value) { completion.set_value(std::move(value)); }
    void unhandled_exception() { completion.set_exception(std::current_exception()); }
  };

  explicit CoroutineTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  CoroutineTask(const CoroutineTask&) = delete;
  CoroutineTask& operator=(const CoroutineTask&) = delete;
  CoroutineTask(CoroutineTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  CoroutineTask& operator=(CoroutineTask&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  ~CoroutineTask() {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::future<Result> future() {
    if (!handle_) {
      throw std::logic_error("cannot obtain a future from an empty coroutine task");
    }
    return handle_.promise().completion.get_future();
  }
  void resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
    }
  }

 private:
  std::coroutine_handle<promise_type> handle_;
};

template <typename Function>
CoroutineTask<std::invoke_result_t<Function>> invoke_coroutine(Function function) {
  static_assert(!std::is_void_v<std::invoke_result_t<Function>>,
                "coroutine functions must return a value");
  static_assert(!std::is_reference_v<std::invoke_result_t<Function>>,
                "coroutine functions must return by value");
  co_return function();
}

}  /* namespace context_hmi::execution */
