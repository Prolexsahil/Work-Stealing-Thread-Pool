#pragma once
// task.hpp - shared building blocks for both pools.
//
// Why not std::function<void()>?  std::function requires the stored callable
// to be *copyable*, but std::packaged_task (which gives us a std::future) is
// move-only.  So we use a tiny move-only, type-erased wrapper instead.

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tp {

class Task {
public:
    Task() = default;

    // Accept any callable except Task itself (so this template never hijacks
    // the move constructor).
    template <class F,
              class = std::enable_if_t<!std::is_same<std::decay_t<F>, Task>::value>>
    explicit Task(F&& f) : impl_(new Model<std::decay_t<F>>(std::forward<F>(f))) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    void operator()() { impl_->run(); }
    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void run() = 0;
    };

    template <class F>
    struct Model final : Concept {
        template <class G>
        explicit Model(G&& g) : fn(std::forward<G>(g)) {}
        void run() override { fn(); }
        F fn;
    };

    std::unique_ptr<Concept> impl_;
};

namespace detail {

inline std::size_t default_workers() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;  // hardware_concurrency() may return 0 if unknown
}

template <class R>
struct Packaged {
    Task task;
    std::future<R> future;
};

// Bind f(args...) into a zero-argument packaged_task and grab its future.
// Arguments are copied/moved into the task (same rule as std::thread);
// use std::ref() to pass by reference.  Exceptions thrown by f are captured
// by the packaged_task and re-thrown from future.get().
template <class F, class... Args>
auto package(F&& f, Args&&... args) {
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    std::packaged_task<R()> pt(
        [fn = std::forward<F>(f),
         tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable -> R {
            return std::apply(std::move(fn), std::move(tup));
        });

    std::future<R> fut = pt.get_future();
    return Packaged<R>{Task(std::move(pt)), std::move(fut)};
}

}  // namespace detail
}  // namespace tp
