// basic_usage.cpp - a quick tour of the API.

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tp/thread_pool.hpp"

long long fib_seq(int n) { return n < 2 ? n : fib_seq(n - 1) + fib_seq(n - 2); }

// Fork-join: split the problem, run one half as a child task, wait with help.
long long fib_par(tp::ThreadPool& pool, int n) {
    if (n < 20) return fib_seq(n);                               // small: do it inline
    auto left = pool.submit(fib_par, std::ref(pool), n - 1);     // fork
    long long right = fib_par(pool, n - 2);
    return pool.wait_and_help(left) + right;                     // join (never deadlocks)
}

int main() {
    tp::ThreadPool pool(4);
    std::cout << "Pool with " << pool.size() << " workers\n";

    // 1. Return a value through a future
    auto sum = pool.submit([] { return 10 + 20; });
    std::cout << "10 + 20 = " << sum.get() << "\n";

    // 2. Pass arguments
    auto greet = pool.submit([](const std::string& name, int n) {
        return "Hello " + name + " x" + std::to_string(n);
    }, std::string("Sahil"), 3);
    std::cout << greet.get() << "\n";

    // 3. Exceptions travel back to the caller
    auto bad = pool.submit([]() -> int { throw std::runtime_error("task failed"); });
    try {
        bad.get();
    } catch (const std::exception& e) {
        std::cout << "Caught from task: " << e.what() << "\n";
    }

    // 4. Many tasks
    std::vector<std::future<int>> squares;
    for (int i = 1; i <= 10; ++i) squares.push_back(pool.submit([i] { return i * i; }));
    int total = 0;
    for (auto& f : squares) total += f.get();
    std::cout << "Sum of squares 1..10 = " << total << "\n";

    // 5. Recursive fork-join
    auto fib = pool.submit(fib_par, std::ref(pool), 32);
    std::cout << "fib(32) = " << pool.wait_and_help(fib) << "\n";

    auto s = pool.stats();
    std::cout << "Tasks executed: " << s.executed << ", stolen: " << s.stolen << "\n";
}   // destructor: graceful shutdown
