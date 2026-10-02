// test_main.cpp - correctness tests for both pools (no external framework).
// Build & run:  g++ -std=c++17 -O2 -pthread -Iinclude tests/test_main.cpp -o tests && ./tests

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "tp/shared_queue_pool.hpp"
#include "tp/thread_pool.hpp"

// ---------------------------------------------------------------- mini framework
namespace {
struct TestCase { std::string name; std::function<void()> fn; };
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
int g_failed_checks = 0;

struct CheckFailure : std::runtime_error { using std::runtime_error::runtime_error; };
}  // namespace

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++g_failed_checks;                                                        \
            throw CheckFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) \
                               + "  CHECK(" #cond ") failed");                        \
        }                                                                             \
    } while (0)

#define CHECK_THROWS_AS(expr, ExType)                     \
    do {                                                  \
        bool thrown_ = false;                             \
        try { expr; } catch (const ExType&) { thrown_ = true; } \
        CHECK(thrown_ && "expected " #ExType);            \
    } while (0)

// Registers a test body once per pool type.
#define TEST_BOTH(name)                                                             \
    template <class Pool> void name();                                              \
    static const bool name##_reg = [] {                                             \
        registry().push_back({#name " [SharedQueuePool]", name<tp::SharedQueuePool>}); \
        registry().push_back({#name " [ThreadPool]", name<tp::ThreadPool>});        \
        return true;                                                                \
    }();                                                                            \
    template <class Pool> void name()

#define TEST(name)                                                   \
    void name();                                                     \
    static const bool name##_reg = [] {                              \
        registry().push_back({#name " [ThreadPool]", name});         \
        return true;                                                 \
    }();                                                             \
    void name()

// ---------------------------------------------------------------- tests for both pools

TEST_BOTH(returns_value) {
    Pool pool(4);
    auto f = pool.submit([] { return 10 + 20; });
    CHECK(f.get() == 30);
}

TEST_BOTH(forwards_arguments) {
    Pool pool(2);
    auto add = [](int a, int b) { return a + b; };
    CHECK(pool.submit(add, 40, 2).get() == 42);

    int x = 0;
    pool.submit([](int& r) { r = 7; }, std::ref(x)).get();  // std::ref passes by reference
    CHECK(x == 7);
}

TEST_BOTH(supports_move_only_types) {
    Pool pool(2);
    auto p = std::make_unique<int>(99);
    auto f = pool.submit([](std::unique_ptr<int> q) { return *q + 1; }, std::move(p));
    CHECK(f.get() == 100);

    auto g = pool.submit([] { return std::make_unique<std::string>("hi"); });
    CHECK(*g.get() == "hi");
}

TEST_BOTH(void_tasks) {
    Pool pool(2);
    std::atomic<int> n{0};
    auto f = pool.submit([&] { n = 5; });
    f.get();
    CHECK(n == 5);
}

TEST_BOTH(exception_propagates_and_worker_survives) {
    Pool pool(1);  // a single worker: if it died, the next task would never run
    auto bad = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(bad.get(), std::runtime_error);
    CHECK(pool.submit([] { return 1; }).get() == 1);
}

TEST_BOTH(many_producers_many_tasks) {
    Pool pool(4);
    constexpr int kProducers = 8, kPerProducer = 5000;
    std::atomic<int> counter{0};
    std::vector<std::thread> producers;
    std::mutex fm;
    std::vector<std::future<void>> futures;
    futures.reserve(kProducers * kPerProducer);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            std::vector<std::future<void>> local;
            local.reserve(kPerProducer);
            for (int i = 0; i < kPerProducer; ++i)
                local.push_back(pool.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }));
            std::lock_guard<std::mutex> lock(fm);
            for (auto& f : local) futures.push_back(std::move(f));
        });
    }
    for (auto& t : producers) t.join();
    for (auto& f : futures) f.get();
    CHECK(counter == kProducers * kPerProducer);
}

TEST_BOTH(shutdown_drains_queue) {
    std::atomic<int> counter{0};
    constexpr int kTasks = 2000;
    {
        Pool pool(2);
        for (int i = 0; i < kTasks; ++i)
            pool.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); });  // futures dropped
        pool.shutdown();
        CHECK(counter == kTasks);  // every queued task ran before shutdown returned
    }
}

TEST_BOTH(submit_after_shutdown_throws) {
    Pool pool(2);
    pool.shutdown();
    CHECK_THROWS_AS(pool.submit([] { return 1; }), std::runtime_error);
}

TEST_BOTH(shutdown_is_idempotent_and_concurrent_safe) {
    Pool pool(3);
    for (int i = 0; i < 100; ++i) pool.submit([] {});
    std::thread a([&] { pool.shutdown(); });
    std::thread b([&] { pool.shutdown(); });
    a.join();
    b.join();
    pool.shutdown();  // third call: no-op
}   // destructor calls shutdown() again: still fine

TEST_BOTH(shutdown_from_worker_is_rejected) {
    Pool pool(2);
    auto f = pool.submit([&] { pool.shutdown(); });
    CHECK_THROWS_AS(f.get(), std::logic_error);
}

TEST_BOTH(zero_workers_rejected) {
    CHECK_THROWS_AS(Pool(0), std::invalid_argument);
}

TEST_BOTH(tasks_run_in_parallel) {
    // 4 tasks that each wait until all 4 have started: only possible if the
    // pool really runs them on 4 threads at the same time.
    Pool pool(4);
    std::atomic<int> started{0};
    std::vector<std::future<bool>> fs;
    for (int i = 0; i < 4; ++i) {
        fs.push_back(pool.submit([&] {
            started.fetch_add(1);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (started.load() < 4)
                if (std::chrono::steady_clock::now() > deadline) return false;
            return true;
        }));
    }
    for (auto& f : fs) CHECK(f.get());
}

// ---------------------------------------------------------------- work-stealing specific

TEST(nested_submission_runs_children) {
    tp::ThreadPool pool(4);
    std::atomic<int> leaves{0};
    std::function<void(int)> spawn = [&](int depth) {
        if (depth == 0) { leaves.fetch_add(1); return; }
        pool.submit(spawn, depth - 1);
        pool.submit(spawn, depth - 1);
    };
    pool.submit(spawn, 10);
    pool.shutdown();          // drain must include children spawned while draining
    CHECK(leaves == 1 << 10);
}

static long long fib_seq(int n) { return n < 2 ? n : fib_seq(n - 1) + fib_seq(n - 2); }

static long long fib_par(tp::ThreadPool& pool, int n) {
    if (n < 12) return fib_seq(n);
    auto left = pool.submit(fib_par, std::ref(pool), n - 1);
    long long right = fib_par(pool, n - 2);
    return pool.wait_and_help(left) + right;  // helps instead of blocking
}

TEST(fork_join_with_one_worker_does_not_deadlock) {
    // With a plain future.get() this deadlocks on a 1-worker pool:
    // the only worker would block waiting for a child that nobody can run.
    tp::ThreadPool pool(1);
    auto f = pool.submit(fib_par, std::ref(pool), 22);
    CHECK(pool.wait_and_help(f) == fib_seq(22));
}

TEST(fork_join_many_workers) {
    tp::ThreadPool pool(4);
    CHECK(pool.wait_and_help(pool.submit(fib_par, std::ref(pool), 25)) == fib_seq(25));
}

TEST(idle_workers_steal_work) {
    tp::ThreadPool pool(4);
    std::mutex m;
    std::set<std::thread::id> ids;
    // One task pushes 64 slow children into its OWN deque; the other three
    // workers only get work by stealing it.
    auto root = pool.submit([&] {
        std::vector<std::future<void>> kids;
        for (int i = 0; i < 64; ++i) {
            kids.push_back(pool.submit([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::lock_guard<std::mutex> lock(m);
                ids.insert(std::this_thread::get_id());
            }));
        }
        for (auto& k : kids) pool.wait_and_help(k);
    });
    root.get();
    CHECK(ids.size() > 1);
    CHECK(pool.stats().stolen > 0);
}

TEST(stats_count_every_task) {
    tp::ThreadPool pool(3);
    for (int i = 0; i < 500; ++i) pool.submit([] {});
    pool.shutdown();
    CHECK(pool.stats().executed == 500);
}

TEST(external_thread_can_help) {
    tp::ThreadPool pool(1);
    std::atomic<bool> release{false};
    // Block the only worker, then let the external (main) thread run the
    // queued task itself via wait_and_help.
    auto blocker = pool.submit([&] { while (!release) std::this_thread::yield(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto f = pool.submit([] { return 5; });
    CHECK(pool.wait_and_help(f) == 5);
    release = true;
    blocker.get();
}

// ---------------------------------------------------------------- runner
int main() {
    int passed = 0, failed = 0;
    for (const auto& tc : registry()) {
        auto t0 = std::chrono::steady_clock::now();
        try {
            tc.fn();
            ++passed;
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("[PASS] %-60s %8.1f ms\n", tc.name.c_str(), ms);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[FAIL] %s\n       %s\n", tc.name.c_str(), e.what());
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
