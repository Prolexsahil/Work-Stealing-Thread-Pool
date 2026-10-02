// benchmark.cpp - measures the three strategies and prints Markdown tables.
//
//   A. thread-per-task       : create a std::thread for every task
//   B. SharedQueuePool       : fixed workers, one shared queue + one mutex
//   C. ThreadPool            : fixed workers, per-worker deques + stealing
//
// Usage:  benchmark            (full run, ~1-3 minutes)
//         benchmark --quick    (smaller sizes, for a fast sanity check)
//
// Every number is the MEDIAN of several repetitions.  Results depend heavily
// on the CPU, OS and compiler, so always quote the machine you ran it on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tp/shared_queue_pool.hpp"
#include "tp/thread_pool.hpp"

using Clock = std::chrono::steady_clock;

namespace {

bool g_quick = false;
int g_reps = 5;

// --------------------------------------------------------------- helpers

// Stop the optimiser from deleting "useless" work.
inline void escape(std::uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r"(v) : "memory");
#else
    static volatile std::uint64_t sink;
    sink = v;
#endif
}

// CPU-bound busy work; `iters` controls the task size.
inline std::uint64_t spin_work(std::uint64_t iters) {
    std::uint64_t x = 0x9E3779B97F4A7C15ull ^ iters;
    for (std::uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

// Wait until `count` tasks have called count_down().
// Only the LAST count_down takes the mutex, and it sets done_ while holding
// it, so wait() cannot return (and destroy the latch) until that thread has
// released the mutex - no use-after-free.
class Latch {
public:
    explicit Latch(long count) : count_(count) {}
    void count_down() {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(m_);
            done_ = true;
            cv_.notify_all();
        }
    }
    void wait() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return done_; });
    }
private:
    std::atomic<long> count_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <class Fn>
double median_ms(Fn&& run_once) {
    run_once();  // warm-up (page faults, thread creation, caches)
    std::vector<double> t;
    for (int r = 0; r < g_reps; ++r) {
        auto t0 = Clock::now();
        run_once();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    return median(t);
}


std::vector<std::size_t> worker_counts(std::size_t hw) {
    std::vector<std::size_t> v;
    for (std::size_t w = 1; w < hw; w *= 2) v.push_back(w);
    v.push_back(hw);
    return v;
}

// --------------------------------------------------------------- strategies

// A: one std::thread per task.  Launched in waves of `hw` threads so that we
// never have thousands of threads alive at once (which would be unfair and
// can exhaust memory on Windows, where each thread reserves a 1 MB stack).
double run_thread_per_task(int n, std::uint64_t iters, std::size_t hw) {
    return median_ms([&] {
        std::vector<std::thread> wave;
        wave.reserve(hw);
        for (int i = 0; i < n;) {
            for (std::size_t k = 0; k < hw && i < n; ++k, ++i)
                wave.emplace_back([iters] { escape(spin_work(iters)); });
            for (auto& t : wave) t.join();
            wave.clear();
        }
    });
}

// B / C: pool is created ONCE outside the timed region (that is the point of
// a pool), then n tasks are submitted from a single producer.
template <class Pool>
double run_pool(Pool& pool, int n, std::uint64_t iters) {
    return median_ms([&] {
        Latch done(n);
        for (int i = 0; i < n; ++i)
            pool.submit([iters, &done] { escape(spin_work(iters)); done.count_down(); });
        done.wait();
    });
}

// Several producer threads submitting at the same time: stresses the
// submission path (shared-queue contention).
template <class Pool>
double run_pool_multi_producer(Pool& pool, int n, std::uint64_t iters, int producers) {
    return median_ms([&] {
        Latch done(n);
        std::vector<std::thread> ps;
        for (int p = 0; p < producers; ++p) {
            ps.emplace_back([&, p] {
                for (int i = p; i < n; i += producers)
                    pool.submit([iters, &done] { escape(spin_work(iters)); done.count_down(); });
            });
        }
        for (auto& t : ps) t.join();
        done.wait();
    });
}

// Recursive spawning: every task spawns 2 children until `depth`; leaves do
// the work.  No task ever waits, so this is safe for both pools.
template <class Pool>
void spawn_tree(Pool& pool, int depth, std::uint64_t iters, Latch& done) {
    if (depth == 0) {
        escape(spin_work(iters));
        done.count_down();
        return;
    }
    pool.submit([&pool, depth, iters, &done] { spawn_tree(pool, depth - 1, iters, done); });
    pool.submit([&pool, depth, iters, &done] { spawn_tree(pool, depth - 1, iters, done); });
}

template <class Pool>
double run_tree(Pool& pool, int depth, std::uint64_t iters) {
    return median_ms([&] {
        Latch done(1L << depth);
        pool.submit([&] { spawn_tree(pool, depth, iters, done); });
        done.wait();
    });
}

// Fork-join (parallel fib with a sequential cutoff) - needs wait_and_help.
long long fib_seq(int n) { return n < 2 ? n : fib_seq(n - 1) + fib_seq(n - 2); }

long long fib_par(tp::ThreadPool& pool, int n, int cutoff) {
    if (n <= cutoff) return fib_seq(n);
    auto left = pool.submit(fib_par, std::ref(pool), n - 1, cutoff);
    long long right = fib_par(pool, n - 2, cutoff);
    return pool.wait_and_help(left) + right;
}

// Queue latency: time from submit() to the task starting.  The producer
// submits one small task every ~`gap_us` microseconds, so workers are mostly
// idle; this measures how quickly a sleeping worker is woken up.
struct Latency { double avg_us, p50_us, p99_us; };

template <class Pool>
Latency run_latency(Pool& pool, int n, double gap_us) {
    std::vector<double> waits(n);
    Latch done(n);
    for (int i = 0; i < n; ++i) {
        const auto submitted = Clock::now();
        pool.submit([&, i, submitted] {
            waits[i] = std::chrono::duration<double, std::micro>(Clock::now() - submitted).count();
            done.count_down();
        });
        const auto until = Clock::now() + std::chrono::duration<double, std::micro>(gap_us);
        while (Clock::now() < until) { /* spin to pace submissions */ }
    }
    done.wait();
    std::sort(waits.begin(), waits.end());
    double sum = 0;
    for (double w : waits) sum += w;
    return {sum / n, waits[n / 2], waits[static_cast<std::size_t>(n * 0.99)]};
}

// --------------------------------------------------------------- reports

void calibrate_and_print_task_sizes(const std::vector<std::uint64_t>& sizes) {
    std::printf("Task sizes (single-thread time per task):\n");
    for (auto it : sizes) {
        const int n = 2000;
        auto t0 = Clock::now();
        for (int i = 0; i < n; ++i) escape(spin_work(it));
        double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / n;
        std::printf("  %7llu iterations ~ %8.2f us\n", static_cast<unsigned long long>(it), us);
    }
    std::printf("\n");
}

void bench_vs_thread_per_task(std::size_t hw) {
    const int n = g_quick ? 2000 : 10000;
    const std::vector<std::uint64_t> sizes = {100, 1000, 10000, 100000};
    calibrate_and_print_task_sizes(sizes);

    std::printf("### 1. Thread-per-task vs thread pools  (%d tasks, %zu workers)\n\n", n, hw);
    std::printf("| Task size (iters) | Thread-per-task (ms) | SharedQueuePool (ms) | ThreadPool (ms) | Speedup vs thread-per-task |\n");
    std::printf("|---:|---:|---:|---:|---:|\n");
    tp::SharedQueuePool shared(hw);
    tp::ThreadPool ws(hw);
    for (auto it : sizes) {
        const int tasks = it >= 100000 ? n / 5 : n;  // keep the big-task runs short
        double a = run_thread_per_task(tasks, it, hw);
        double b = run_pool(shared, tasks, it);
        double c = run_pool(ws, tasks, it);
        std::printf("| %llu%s | %.1f | %.1f | %.1f | %.1fx |\n",
                    static_cast<unsigned long long>(it), tasks != n ? " (n/5 tasks)" : "",
                    a, b, c, a / std::min(b, c));
        std::fflush(stdout);
    }
    std::printf("\n");
}

void bench_scaling(std::size_t hw) {
    const int n = g_quick ? 20000 : 100000;
    const std::uint64_t iters = 100;  // tiny tasks -> queue overhead dominates
    const int producers = 4;
    std::printf("### 2. Worker scaling with tiny tasks  (%d tasks, %d producer threads)\n\n", n, producers);
    std::printf("| Workers | SharedQueuePool (tasks/s) | ThreadPool (tasks/s) | ThreadPool / Shared |\n");
    std::printf("|---:|---:|---:|---:|\n");
    for (auto w : worker_counts(hw)) {
        tp::SharedQueuePool shared(w);
        tp::ThreadPool ws(w);
        double b = run_pool_multi_producer(shared, n, iters, producers);
        double c = run_pool_multi_producer(ws, n, iters, producers);
        std::printf("| %zu | %.0f | %.0f | %.2fx |\n", w, n / (b / 1000.0), n / (c / 1000.0), b / c);
        std::fflush(stdout);
    }
    std::printf("\n");
}

void bench_recursive(std::size_t hw) {
    const int depth = g_quick ? 14 : 17;  // 2^depth leaf tasks
    const std::uint64_t iters = 200;
    std::printf("### 3. Recursive task spawning  (binary tree, 2^%d = %d leaf tasks, %zu workers)\n\n",
                depth, 1 << depth, hw);
    std::printf("| Pool | Time (ms) | Tasks/s | Steals |\n|---|---:|---:|---:|\n");
    tp::SharedQueuePool shared(hw);
    tp::ThreadPool ws(hw);
    const int total_tasks = (1 << (depth + 1)) - 1;
    double b = run_tree(shared, depth, iters);
    auto before = ws.stats().stolen;
    double c = run_tree(ws, depth, iters);
    auto steals = (ws.stats().stolen - before) / (g_reps + 1);
    std::printf("| SharedQueuePool | %.1f | %.0f | - |\n", b, total_tasks / (b / 1000.0));
    std::printf("| ThreadPool | %.1f | %.0f | ~%llu per run |\n\n", c, total_tasks / (c / 1000.0),
                static_cast<unsigned long long>(steals));
}

void bench_fork_join(std::size_t hw) {
    const int n = g_quick ? 32 : 38, cutoff = 18;
    std::printf("### 4. Fork-join parallel fib(%d)  (cutoff %d, ThreadPool + wait_and_help)\n\n", n, cutoff);
    std::printf("SharedQueuePool cannot run this workload: parents block on children and\n");
    std::printf("the pool deadlocks (see examples/deadlock_demo.cpp).\n\n");
    double seq = median_ms([&] { escape(static_cast<std::uint64_t>(fib_seq(n))); });
    std::printf("| Workers | Time (ms) | Speedup vs sequential |\n|---:|---:|---:|\n");
    std::printf("| sequential | %.1f | 1.00x |\n", seq);
    for (auto w : worker_counts(hw)) {
        tp::ThreadPool ws(w);
        double t = median_ms([&] {
            escape(static_cast<std::uint64_t>(ws.wait_and_help(ws.submit(fib_par, std::ref(ws), n, cutoff))));
        });
        std::printf("| %zu | %.1f | %.2fx |\n", w, t, seq / t);
        std::fflush(stdout);
    }
    std::printf("\n");
}

void bench_latency(std::size_t hw) {
    const int n = g_quick ? 1000 : 5000;
    const double gap_us = 50;
    std::printf("### 5. Submit-to-start latency, lightly loaded  (%d tasks, one every ~%.0f us, %zu workers)\n\n",
                n, gap_us, hw);
    std::printf("| Pool | Avg (us) | p50 (us) | p99 (us) |\n|---|---:|---:|---:|\n");
    tp::SharedQueuePool shared(hw);
    tp::ThreadPool ws(hw);
    run_latency(shared, n / 10, gap_us);  // warm-up
    run_latency(ws, n / 10, gap_us);
    auto b = run_latency(shared, n, gap_us);
    auto c = run_latency(ws, n, gap_us);
    std::printf("| SharedQueuePool | %.1f | %.1f | %.1f |\n", b.avg_us, b.p50_us, b.p99_us);
    std::printf("| ThreadPool | %.1f | %.1f | %.1f |\n\n", c.avg_us, c.p50_us, c.p99_us);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--quick") == 0) g_quick = true;
    if (g_quick) g_reps = 3;

    const std::size_t hw = tp::detail::default_workers();
    std::printf("# Thread pool benchmarks\n\n");
    std::printf("Hardware threads: %zu | repetitions: %d (median reported) | mode: %s\n\n",
                hw, g_reps, g_quick ? "quick" : "full");

    bench_vs_thread_per_task(hw);
    bench_scaling(hw);
    bench_recursive(hw);
    bench_fork_join(hw);
    bench_latency(hw);
    std::printf("Done.\n");
}
