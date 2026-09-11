/**
 * Bounded worker pool and coroutine task execution.
 *
 * Contract under test: submission order is preserved for a single worker, a full queue is
 * refused rather than silently dropped, an exception inside a job reaches the caller through
 * its future, shutdown drains queued work instead of discarding it, and the accounting adds
 * up so that no task is lost.
 *
 * Every wait in this file is bounded. A test that hangs is a failed test, not a stuck suite.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "context_hmi/worker_pool.hpp"
#include "support/check.hpp"

using context_hmi::execution::CoroutineTask;
using context_hmi::execution::invoke_coroutine;
using context_hmi::execution::QueueFull;
using context_hmi::execution::WorkerPool;
using context_hmi_test::check;

namespace {

using Milliseconds = std::chrono::milliseconds;

/** Wait for a condition with a hard deadline so a broken pool fails instead of hanging. */
template <typename Predicate>
bool wait_for(Predicate predicate, Milliseconds limit = Milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(Milliseconds(1));
    }
    return predicate();
}

template <typename Result> bool ready(std::future<Result> &future) {
    return future.wait_for(Milliseconds(5000)) == std::future_status::ready;
}

std::size_t counter_of(const WorkerPool &pool, const char *name) {
    return pool.snapshot().at(name).get<std::size_t>();
}

void a_pool_requires_positive_bounds() {
    bool threw = false;
    try {
        WorkerPool pool(0, 4);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "a pool with no worker is rejected");

    threw = false;
    try {
        WorkerPool pool(1, 0);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "a pool with no queue capacity is rejected");
}

void a_result_reaches_the_caller() {
    WorkerPool pool(2, 8);
    auto future = pool.submit([] { return 41 + 1; });
    check(ready(future), "a submitted job completes within the deadline");
    check(future.get() == 42, "the job result reaches the caller through its future");
    check(counter_of(pool, "submitted") == 1, "the pool accounts for the submission");
    check(wait_for([&] { return counter_of(pool, "completed") == 1; }),
          "the pool accounts for the completion");
}

/** One worker consuming a FIFO queue must run jobs in submission order. */
void submission_order_is_preserved_for_one_worker() {
    WorkerPool pool(1, 64);
    std::mutex mutex;
    std::vector<int> order;
    std::vector<std::future<int>> futures;
    constexpr int kJobs = 32;

    for (int index = 0; index < kJobs; ++index) {
        futures.push_back(pool.submit([&mutex, &order, index] {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(index);
            return index;
        }));
    }
    for (auto &future : futures)
        check(ready(future), "every ordered job completes");

    std::lock_guard<std::mutex> lock(mutex);
    check(order.size() == static_cast<std::size_t>(kJobs), "every ordered job ran exactly once");
    bool in_order = true;
    for (std::size_t index = 0; index < order.size(); ++index)
        if (order[index] != static_cast<int>(index))
            in_order = false;
    check(in_order, "a single worker preserves submission order");
}

/** A full queue is refused with a named error and counted, never silently dropped. */
void a_full_queue_refuses_rather_than_drops() {
    WorkerPool pool(1, 2);
    std::atomic<bool> gate{false};

    auto blocking = pool.submit([&gate] {
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::sleep_for(Milliseconds(1));
        return 0;
    });
    check(wait_for([&] { return counter_of(pool, "active") == 1; }),
          "the worker picked up the blocking job, so the queue is empty");

    auto queued_first = pool.submit([] { return 1; });
    auto queued_second = pool.submit([] { return 2; });
    check(wait_for([&] { return counter_of(pool, "queue_depth") == 2; }),
          "the queue filled to its declared capacity");

    bool refused = false;
    try {
        auto rejected = pool.submit([] { return 3; });
        check(false, "a submission beyond the queue capacity must not be accepted");
    } catch (const QueueFull &) {
        refused = true;
    }
    check(refused, "a full queue refuses the submission with QueueFull");
    check(counter_of(pool, "rejected") == 1, "the refusal is counted");
    check(counter_of(pool, "submitted") == 3, "a refused submission is not counted as submitted");

    gate.store(true, std::memory_order_release);
    check(ready(blocking) && ready(queued_first) && ready(queued_second),
          "every accepted job still completes after the queue drains");
    check(queued_first.get() == 1 && queued_second.get() == 2,
          "the accepted jobs return their own results");
    check(wait_for([&] { return counter_of(pool, "completed") == 3; }),
          "exactly the accepted jobs completed");
}

/** An exception inside a job must surface at the caller, not terminate a worker. */
void an_exception_propagates_through_the_future() {
    WorkerPool pool(2, 8);
    auto failing = pool.submit([]() -> int { throw std::runtime_error("job failed"); });
    check(ready(failing), "a failing job still completes its future");

    std::string message;
    try {
        (void)failing.get();
        check(false, "a throwing job must not report success");
    } catch (const std::runtime_error &error) {
        message = error.what();
    }
    check(message == "job failed", "the original exception message reaches the caller");

    auto healthy = pool.submit([] { return 7; });
    check(ready(healthy) && healthy.get() == 7,
          "the worker survives a failed job and keeps serving");
}

/** Shutdown must drain what was already accepted. Accepted work is never discarded. */
void shutdown_drains_queued_work() {
    std::vector<std::future<int>> futures;
    std::atomic<bool> gate{false};
    std::size_t completed = 0;
    std::size_t submitted = 0;

    {
        WorkerPool pool(1, 16);
        futures.push_back(pool.submit([&gate] {
            while (!gate.load(std::memory_order_acquire))
                std::this_thread::sleep_for(Milliseconds(1));
            return 0;
        }));
        check(wait_for([&] { return counter_of(pool, "active") == 1; }),
              "the worker is busy before the queue is filled");

        for (int index = 1; index <= 5; ++index)
            futures.push_back(pool.submit([index] { return index; }));
        check(counter_of(pool, "queue_depth") == 5, "five jobs are waiting when shutdown begins");

        gate.store(true, std::memory_order_release);
        pool.shutdown();
        completed = counter_of(pool, "completed");
        submitted = counter_of(pool, "submitted");

        bool refused = false;
        try {
            auto rejected = pool.submit([] { return 99; });
        } catch (const QueueFull &) {
            refused = true;
        }
        check(refused, "a pool that has shut down accepts no further work");
    }

    check(submitted == 6, "every accepted job was counted as submitted");
    check(completed == 6, "shutdown drained the queue instead of discarding accepted work");
    for (std::size_t index = 0; index < futures.size(); ++index) {
        const std::string label = "queued job " + std::to_string(index);
        if (!futures[index].valid()) {
            check(false, label + " kept its future");
            continue;
        }
        /* Read the value only once it is settled, so a pool that discards work reports a
           failed assertion instead of terminating on a broken promise. */
        if (futures[index].wait_for(Milliseconds(2000)) != std::future_status::ready) {
            check(false, label + " completed before shutdown returned");
            continue;
        }
        try {
            check(futures[index].get() == static_cast<int>(index),
                  label + " produced its own result");
        } catch (const std::exception &error) {
            check(false, label + " was discarded by shutdown: " + error.what());
        }
    }
}

/** Under concurrent producers the accounting must balance exactly. */
void concurrent_submission_loses_no_task() {
    WorkerPool pool(4, 32);
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 25;

    std::mutex mutex;
    std::vector<std::future<int>> futures;
    std::atomic<std::size_t> refused{0};
    std::vector<std::thread> producers;

    for (std::size_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&] {
            for (std::size_t index = 0; index < kPerProducer; ++index) {
                try {
                    auto future = pool.submit([] { return 1; });
                    std::lock_guard<std::mutex> lock(mutex);
                    futures.push_back(std::move(future));
                } catch (const QueueFull &) {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &producer : producers)
        producer.join();

    std::size_t accepted = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        accepted = futures.size();
        for (auto &future : futures)
            check(ready(future), "every accepted concurrent job completes");
    }

    const std::size_t attempts = kProducers * kPerProducer;
    check(accepted + refused.load() == attempts,
          "every attempt was either accepted or explicitly refused");
    check(counter_of(pool, "submitted") == accepted, "submitted matches the accepted count");
    check(wait_for([&] { return counter_of(pool, "completed") == accepted; }),
          "completed matches the accepted count, so no accepted task was lost");
    check(counter_of(pool, "rejected") == refused.load(),
          "rejected matches the refusals observed by the producers");
}

/** The coroutine task carries a value or an exception, and only runs when resumed. */
void a_coroutine_task_carries_its_result() {
    auto task = invoke_coroutine([] { return 41 + 1; });
    auto future = task.future();
    check(future.wait_for(Milliseconds(0)) != std::future_status::ready,
          "a coroutine task does not run before it is resumed");
    task.resume();
    check(future.wait_for(Milliseconds(0)) == std::future_status::ready,
          "resuming the coroutine completes its future");
    check(future.get() == 42, "the coroutine result reaches the caller");

    auto failing = invoke_coroutine([]() -> int { throw std::runtime_error("coroutine failed"); });
    auto failing_future = failing.future();
    failing.resume();
    std::string message;
    try {
        (void)failing_future.get();
        check(false, "a throwing coroutine must not report success");
    } catch (const std::runtime_error &error) {
        message = error.what();
    }
    check(message == "coroutine failed", "the coroutine exception reaches the caller intact");
}

void the_pool_snapshot_describes_its_own_bounds() {
    WorkerPool pool(2, 8);
    const auto snapshot = pool.snapshot();
    check(snapshot.at("workers") == 2, "the snapshot reports the worker count");
    check(snapshot.at("queue_capacity") == 8, "the snapshot reports the declared capacity");
    check(snapshot.at("accepting") == true, "a running pool reports that it accepts work");
    for (const char *field : {"queue_depth", "active", "submitted", "completed", "rejected"})
        check(snapshot.contains(field) && snapshot.at(field).is_number(),
              std::string("the snapshot reports ") + field + " as a number");
}

}  /* namespace */

int main() {
    a_pool_requires_positive_bounds();
    a_result_reaches_the_caller();
    submission_order_is_preserved_for_one_worker();
    a_full_queue_refuses_rather_than_drops();
    an_exception_propagates_through_the_future();
    shutdown_drains_queued_work();
    concurrent_submission_loses_no_task();
    a_coroutine_task_carries_its_result();
    the_pool_snapshot_describes_its_own_bounds();
    return context_hmi_test::report("execution tests");
}
