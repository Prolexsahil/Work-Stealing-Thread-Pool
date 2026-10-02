// deadlock_demo.cpp - the nested-wait deadlock, and how wait_and_help fixes it.
//
// A pool with ONE worker runs a parent task.  The parent submits a child task
// and waits for it.  The only worker is busy running the (blocked) parent, so
// nobody can run the child: DEADLOCK.  The same happens with N workers when
// N parents wait at the same time (e.g. recursive fork-join algorithms).
//
// So that this demo can finish, the naive version waits with a timeout
// instead of a plain future.get() and reports the deadlock.

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "tp/shared_queue_pool.hpp"
#include "tp/thread_pool.hpp"

using namespace std::chrono_literals;

template <class Pool>
void naive_blocking_wait(const char* name) {
    Pool pool(1);
    auto parent = pool.submit([&pool] {
        auto child = pool.submit([] { return 42; });
        if (child.wait_for(1s) == std::future_status::timeout)  // stands in for child.get()
            throw std::runtime_error("DEADLOCK - child never ran (timed out after 1s)");
        return child.get();
    });
    std::string outcome;
    try {
        outcome = "result = " + std::to_string(parent.get());
    } catch (const std::exception& e) {
        outcome = e.what();
    }
    std::cout << name << " + blocking wait:  " << outcome << "\n";
}

void work_stealing_help() {
    tp::ThreadPool pool(1);
    auto parent = pool.submit([&pool] {
        auto child = pool.submit([] { return 42; });
        return pool.wait_and_help(child);  // runs the child itself while waiting
    });
    std::cout << "ThreadPool      + wait_and_help(): result = " << parent.get() << "\n";
}

int main() {
    naive_blocking_wait<tp::SharedQueuePool>("SharedQueuePool");
    naive_blocking_wait<tp::ThreadPool>("ThreadPool     ");
    work_stealing_help();
}
