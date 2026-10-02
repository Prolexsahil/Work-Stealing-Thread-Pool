#pragma once
// shared_queue_pool.hpp - the classic thread pool, used as the baseline.
//
//   producers --push--> [ one deque + one mutex ] <--pop-- workers
//
// Simple and correct, but every submit AND every pop fights over the same
// mutex, so it becomes a bottleneck as the number of workers grows.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "tp/task.hpp"

namespace tp {

class SharedQueuePool {
public:
    explicit SharedQueuePool(std::size_t num_workers = detail::default_workers()) {
        if (num_workers == 0) throw std::invalid_argument("SharedQueuePool: num_workers must be > 0");
        workers_.reserve(num_workers);
        try {
            for (std::size_t i = 0; i < num_workers; ++i)
                workers_.emplace_back([this] { worker_loop(); });
        } catch (...) {
            // If creating thread k fails, threads 0..k-1 are already running.
            // Destroying a joinable std::thread calls std::terminate, so stop
            // and join them before re-throwing.
            shutdown();
            throw;
        }
    }

    ~SharedQueuePool() { shutdown(); }

    SharedQueuePool(const SharedQueuePool&) = delete;
    SharedQueuePool& operator=(const SharedQueuePool&) = delete;

    // Submit f(args...) and get a std::future for its result.
    // Throws std::runtime_error if the pool has been shut down.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) {
        auto packaged = detail::package(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("SharedQueuePool: submit() after shutdown()");
            queue_.push_back(std::move(packaged.task));
        }
        cv_.notify_one();  // wake one sleeping worker (outside the lock)
        return std::move(packaged.future);
    }

    // Graceful shutdown: stop accepting new tasks, finish every queued task,
    // then join all workers.  Safe to call more than once / from several
    // threads.  Must not be called from inside one of this pool's tasks.
    void shutdown() {
        if (current_pool_ == this)
            throw std::logic_error("SharedQueuePool: shutdown() from a worker thread would deadlock");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        current_pool_ = this;
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // The predicate protects against spurious wakeups and lost
                // wakeups: we only sleep if there is truly nothing to do.
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping_ and fully drained
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            // Run the task WITHOUT holding the lock, otherwise only one
            // worker could make progress at a time.
            try {
                task();
            } catch (...) {
                // packaged_task already stores exceptions in the future;
                // this is only a safety net so a worker can never die.
            }
        }
    }

    std::mutex mutex_;              // guards queue_ and stopping_
    std::condition_variable cv_;
    std::deque<Task> queue_;
    bool stopping_ = false;
    std::mutex join_mutex_;         // serialises concurrent shutdown() calls
    std::vector<std::thread> workers_;

    inline static thread_local SharedQueuePool* current_pool_ = nullptr;
};

}  // namespace tp
