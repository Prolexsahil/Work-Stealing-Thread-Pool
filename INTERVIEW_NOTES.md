# Interview Notes: Work-Stealing Thread Pool

Read this before any interview where the project is on your resume. Each answer is short
enough to say out loud.

## The 30-second pitch

"I built a header-only C++17 thread pool library. Instead of one shared task queue, every
worker has its own deque; workers take their own newest task and, when idle, steal the oldest
task from another worker. That cuts lock contention and keeps recursive workloads local. It
returns results through std::future, propagates exceptions, shuts down gracefully, and has a
wait_and_help function that lets fork-join algorithms wait for child tasks without deadlocking.
I compared it against a thread-per-task approach and a classic shared-queue pool with a
benchmark suite, and verified it with ThreadSanitizer."

## Basics

**Why use a thread pool at all?**
Creating and destroying an OS thread is expensive (a kernel call, a stack allocation, scheduler
bookkeeping). A pool pays that cost once and reuses threads. It also caps the number of threads,
so the machine is not oversubscribed.

**How many workers should a pool have?**
For CPU-bound work, about the number of hardware threads (`std::thread::hardware_concurrency()`).
More threads than cores just adds context switches. For I/O-bound work you may want more,
because threads spend time blocked.

**Thread vs process?**
Threads in one process share the address space (heap, globals, code) but each has its own stack
and registers. Processes have separate address spaces, so they are isolated but communicate
more expensively.

## Synchronization

**Why a mutex AND a condition variable?**
The mutex protects the queue from concurrent modification. The condition variable lets an idle
worker sleep instead of burning CPU in a loop, and be woken when work arrives.

**What is a spurious wakeup and how do you handle it?**
A thread can return from `wait()` even though nobody notified it. We always wait with a
predicate, `cv.wait(lock, [&]{ return stopping || !queue.empty(); })`, which re-checks the
condition and goes back to sleep if it is false.

**What is a lost wakeup?**
A worker checks "queue empty", and before it actually starts waiting, a producer pushes a task
and notifies. The notification is lost and the worker sleeps with work available. Checking
the condition and waiting while holding the same mutex that the producer uses to change the
condition prevents it.

**Why run the task outside the lock?**
If a worker held the queue lock while running a task, no other worker could pop, so the pool
would execute one task at a time.

**How does your work-stealing pool avoid lost wakeups without locking on every submit?**
A counter `pending_` counts queued tasks, and `sleepers_` counts sleeping workers. The submitter
does `pending_++` then reads `sleepers_`; the worker does `sleepers_++` then reads `pending_`.
With sequentially consistent atomics, at least one of them sees the other's write, so either the
worker notices the task and doesn't sleep, or the submitter notices the sleeper and wakes it.

## Work stealing

**Why does the owner take from the back but thieves take from the front?**
The owner's newest task is most likely still in its CPU cache (LIFO). Thieves take the oldest
task, which in recursive algorithms is usually the biggest chunk of remaining work, so one steal
moves a lot of work and steals stay rare. The two ends also reduce contention between owner and thief.

**Why does work stealing help?**
With one shared queue, every submit and every pop fights over one lock, and that lock becomes the
bottleneck as workers increase. With per-worker deques, a worker usually touches only its own
lock; it only touches others when it runs out of work.

**Real systems that use it:** Intel TBB, Java's ForkJoinPool, Go's goroutine scheduler, Rust's
Tokio and Rayon.

**What is false sharing and how did you avoid it?**
Two threads writing different variables that sit on the same 64-byte cache line force that line
to bounce between cores. Each worker's deque and counters are `alignas(64)`, so they sit on
separate cache lines.

## Deadlocks and shutdown

**Explain the nested-submission deadlock.**
If every worker runs a task that calls `future.get()` on a child task still waiting in a queue,
there is no free worker to run the children, so everything waits forever. My demo shows it with
one worker. `wait_and_help` fixes it by running queued tasks while waiting, so the waiting
worker can run the child itself.

**The four conditions for deadlock?**
Mutual exclusion, hold and wait, no preemption, and circular wait. Break any one to prevent it.

**How does graceful shutdown work?**
Set `stopping_` under the lock, notify all workers, then join them. Workers exit only when
stopping is set AND no tasks are pending, so everything queued finishes first. New external
submits throw an exception. Calling `shutdown()` from inside a task would make a thread join
itself, so it throws `std::logic_error` instead.

**What happens if thread creation fails in the constructor?**
Threads that already started are joined before re-throwing, because destroying a joinable
`std::thread` calls `std::terminate`.

## C++ details

**Why not `std::function<void()>` for tasks?**
`std::function` requires a copyable callable, but `std::packaged_task` is move-only. I wrote a
small type-erased, move-only `Task` class (a `unique_ptr` to a virtual base, with a templated
derived class holding the callable).

**How do exceptions reach the caller?**
`std::packaged_task` catches any exception from the function and stores it in the shared state;
`future.get()` re-throws it on the caller's thread.

**Why did you use `std::ref` in the examples?**
Arguments are copied into the task (same rule as `std::thread`), so passing by reference needs
`std::ref`.

## Measurement

**How did you benchmark fairly?**
Pools are created outside the timed region, there is a warm-up run, the median of 5 runs is
reported, work is kept from being optimized away, and task size is varied. Thread-per-task is
launched in waves of N threads so it is not unfairly flooded.

**Is the pool always faster than creating threads?**
No. For large tasks, thread creation cost is small relative to the work, so the difference
shrinks. The pool wins most for many small tasks. That's why the benchmark varies task size.

**How did you check correctness?**
A 30-case test suite, including stress tests with 8 concurrent producers, run repeatedly under
ThreadSanitizer (which detects data races) and AddressSanitizer.

## What I'd do next

- A lock-free Chase–Lev deque (needs careful memory ordering).
- Task priorities.
- A dynamically sized pool.
- Cancellation.
- Keeping blocking I/O tasks off the CPU workers.
