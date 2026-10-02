#pragma once
// thread_pool.hpp - a work-stealing thread pool.
//
// Each worker owns its own deque protected by its own mutex:
//
//   worker k:  pops from the BACK of its own deque   (LIFO: hot in cache)
//   thieves:   steal from the FRONT of other deques  (FIFO: oldest, usually
//                                                     the biggest chunk of work)
//
// * Tasks submitted from outside the pool are spread round-robin.
// * Tasks submitted from inside a worker go to that worker's own deque, so
//   recursive / fork-join workloads never touch a shared lock on the hot path.
// * wait_and_help(future) lets a task wait for a child task while running
//   other queued tasks, which avoids the classic nested-wait deadlock.
//
// Sleeping: idle workers block on a condition variable.  pending_ counts
// tasks that have been reserved/pushed but not yet popped; a worker only
// sleeps when pending_ == 0, and only exits when stopping_ && pending_ == 0.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "tp/task.hpp"

namespace tp {

class ThreadPool {
public:
    struct Stats {
        std::uint64_t executed = 0;  // tasks run (by workers and helpers)
        std::uint64_t stolen = 0;    // tasks taken from another worker's deque
    };

    explicit ThreadPool(std::size_t num_workers = detail::default_workers()) {
        if (num_workers == 0) throw std::invalid_argument("ThreadPool: num_workers must be > 0");
        queues_.reserve(num_workers);
        for (std::size_t i = 0; i < num_workers; ++i)
            queues_.push_back(std::make_unique<WorkerQueue>());
        counters_ = std::make_unique<Counters[]>(num_workers + 1);  // last slot: external helpers

        workers_.reserve(num_workers);
        try {
            for (std::size_t i = 0; i < num_workers; ++i)
                workers_.emplace_back(&ThreadPool::worker_loop, this, i);
        } catch (...) {
            shutdown();  // join the threads that did start (see SharedQueuePool)
            throw;
        }
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit f(args...) and get a std::future for its result.
    // From outside the pool: throws std::runtime_error after shutdown().
    // From inside a task: always accepted (a running task may need to spawn
    // children even while the pool is draining).
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) {
        auto packaged = detail::package(std::forward<F>(f), std::forward<Args>(args)...);
        if (current_pool_ == this)
            submit_internal(std::move(packaged.task));
        else
            submit_external(std::move(packaged.task));
        return std::move(packaged.future);
    }

    // Wait for `fut`, running other queued tasks while it is not ready.
    // Use this instead of fut.get() inside a task that waits on a child task.
    template <class T>
    T wait_and_help(std::future<T>& fut) {
        const std::size_t self = (current_pool_ == this) ? current_index_ : kExternal;
        while (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (!try_run_one(self)) std::this_thread::yield();
        }
        return fut.get();
    }

    template <class T>
    T wait_and_help(std::future<T>&& fut) {
        return wait_and_help(fut);
    }

    // Graceful shutdown: reject new external tasks, finish all queued tasks
    // (including children they spawn), then join all workers.  Idempotent.
    void shutdown() {
        if (current_pool_ == this)
            throw std::logic_error("ThreadPool: shutdown() from a worker thread would deadlock");
        {
            std::lock_guard<std::mutex> lock(sleep_mutex_);
            stopping_ = true;
        }
        sleep_cv_.notify_all();
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    std::size_t size() const noexcept { return workers_.size(); }

    Stats stats() const noexcept {
        Stats s;
        for (std::size_t i = 0; i <= queues_.size(); ++i) {
            s.executed += counters_[i].executed.load(std::memory_order_relaxed);
            s.stolen += counters_[i].stolen.load(std::memory_order_relaxed);
        }
        return s;
    }

private:
    static constexpr std::size_t kExternal = static_cast<std::size_t>(-1);

    // alignas(64): keep each worker's hot data on its own cache line to avoid
    // false sharing between cores.
    struct alignas(64) WorkerQueue {
        std::mutex mutex;
        std::deque<Task> tasks;
    };
    struct alignas(64) Counters {
        std::atomic<std::uint64_t> executed{0};
        std::atomic<std::uint64_t> stolen{0};
    };

    // Called by a thread that is not one of our workers.
    void submit_external(Task&& task) {
        bool wake;
        {
            std::lock_guard<std::mutex> lock(sleep_mutex_);
            if (stopping_) throw std::runtime_error("ThreadPool: submit() after shutdown()");
            // Reserve BEFORE pushing, so pending_ never undercounts the tasks
            // sitting in the deques (a worker must not exit or sleep while a
            // task exists).
            pending_.fetch_add(1, std::memory_order_seq_cst);
            const std::size_t q = next_queue_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
            try {
                std::lock_guard<std::mutex> qlock(queues_[q]->mutex);
                queues_[q]->tasks.push_back(std::move(task));
            } catch (...) {
                pending_.fetch_sub(1, std::memory_order_seq_cst);  // e.g. bad_alloc
                throw;
            }
            wake = sleepers_.load(std::memory_order_seq_cst) > 0;
        }
        if (wake) sleep_cv_.notify_one();
    }

    // Called by one of our own workers (e.g. a task spawning child tasks).
    // Fast path: no global lock, only the worker's own deque mutex.
    void submit_internal(Task&& task) {
        pending_.fetch_add(1, std::memory_order_seq_cst);
        try {
            WorkerQueue& q = *queues_[current_index_];
            std::lock_guard<std::mutex> qlock(q.mutex);
            q.tasks.push_back(std::move(task));
        } catch (...) {
            pending_.fetch_sub(1, std::memory_order_seq_cst);
            throw;
        }
        // Lost-wakeup protection without taking the lock every time:
        //   submitter: pending_++  then  read sleepers_
        //   worker:    sleepers_++ then  read pending_   (inside the lock)
        // With seq_cst, at least one side sees the other's write: either the
        // worker sees pending_ > 0 and does not sleep, or we see a sleeper and
        // wake it.  Taking the lock before notify guarantees the worker has
        // actually started waiting (it holds the lock until it blocks).
        if (sleepers_.load(std::memory_order_seq_cst) > 0) {
            { std::lock_guard<std::mutex> lock(sleep_mutex_); }
            sleep_cv_.notify_one();
        }
    }

    bool pop_local(std::size_t self, Task& out) {
        WorkerQueue& q = *queues_[self];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.tasks.empty()) return false;
        out = std::move(q.tasks.back());  // LIFO for the owner
        q.tasks.pop_back();
        return true;
    }

    bool steal(std::size_t self, Task& out) {
        const std::size_t n = queues_.size();
        const std::size_t start =
            (self == kExternal) ? next_victim_.fetch_add(1, std::memory_order_relaxed) : self;
        for (std::size_t i = 1; i <= n; ++i) {
            const std::size_t victim = (start + i) % n;
            if (victim == self) continue;
            WorkerQueue& q = *queues_[victim];
            std::lock_guard<std::mutex> lock(q.mutex);
            if (q.tasks.empty()) continue;
            out = std::move(q.tasks.front());  // FIFO for thieves
            q.tasks.pop_front();
            return true;
        }
        return false;
    }

    // Find one task (own deque first, then steal) and run it.
    bool try_run_one(std::size_t self) {
        Task task;
        bool stolen = false;
        if (self != kExternal && pop_local(self, task)) {
            // got one from our own deque
        } else if (steal(self, task)) {
            stolen = true;
        } else {
            return false;
        }
        pending_.fetch_sub(1, std::memory_order_seq_cst);

        Counters& c = counters_[self == kExternal ? queues_.size() : self];
        if (stolen) c.stolen.fetch_add(1, std::memory_order_relaxed);
        try {
            task();
        } catch (...) {
            // packaged_task stores exceptions in the future; safety net only.
        }
        c.executed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void worker_loop(std::size_t self) {
        current_pool_ = this;
        current_index_ = self;
        for (;;) {
            if (try_run_one(self)) continue;

            std::unique_lock<std::mutex> lock(sleep_mutex_);
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            sleep_cv_.wait(lock, [this] {
                return stopping_ || pending_.load(std::memory_order_seq_cst) > 0;
            });
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
            if (stopping_ && pending_.load(std::memory_order_seq_cst) == 0) return;
        }
    }

    std::vector<std::unique_ptr<WorkerQueue>> queues_;
    std::unique_ptr<Counters[]> counters_;

    std::atomic<std::size_t> pending_{0};    // reserved/queued, not yet popped
    std::atomic<std::size_t> sleepers_{0};   // workers blocked on sleep_cv_
    std::atomic<std::size_t> next_queue_{0}; // round-robin for external submits
    std::atomic<std::size_t> next_victim_{0};// steal start for external helpers

    std::mutex sleep_mutex_;                 // guards stopping_ and the sleep/wake handshake
    std::condition_variable sleep_cv_;
    bool stopping_ = false;

    std::mutex join_mutex_;
    std::vector<std::thread> workers_;

    inline static thread_local ThreadPool* current_pool_ = nullptr;
    inline static thread_local std::size_t current_index_ = 0;
};

}  // namespace tp
