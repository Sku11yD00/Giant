// Job system: the only way engine modules use more than one CPU core.
//
//   JobSystem jobs;                       // one worker per core, minus one
//   WaitGroup done;
//   jobs.submit(done, [] { stream_cell(); });
//   jobs.parallel_for(cells.size(), 16, [&](usize begin, usize end) { ... });
//   jobs.wait(done);                      // the waiting thread helps run jobs
//
// Jobs must not throw. Destroying the JobSystem runs every job still queued.
//
// submit() never waits for another thread: the job is pushed onto a list no lock guards and
// one sleeping thread is woken. A frame's thread hands work over many times a frame; behind
// a lock it shared with the workers, it stood still for as long as a worker that held the
// lock was kept off its core (milliseconds, on a busy machine). wait() and parallel_for()
// do take the queue's lock, as the workers do, for a few pointers' work.
#pragma once

#include "giant/core/types.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace giant {

// Counts outstanding jobs. A job submitted with a WaitGroup decrements it when
// it finishes; JobSystem::wait() returns once it reaches zero.
class WaitGroup {
public:
    WaitGroup() = default;
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;

    bool done() const { return m_pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<i64> m_pending{0};
};

class JobSystem {
public:
    // worker_count 0 means "hardware threads minus one" (at least one).
    explicit JobSystem(u32 worker_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    u32 worker_count() const { return static_cast<u32>(m_workers.size()); }

    void submit(WaitGroup& group, std::function<void()> job);

    // Runs fn(begin, end) over [0, count) in chunks of at most batch_size,
    // spread over the workers and the calling thread. Returns when all are done.
    void parallel_for(usize count, usize batch_size,
                      const std::function<void(usize begin, usize end)>& fn);

    // Blocks until the group is done. While waiting, the thread runs any
    // queued job (not only the group's), so waiting from inside a job cannot
    // starve the pool. Rule: a job may only wait on work it submitted itself,
    // never on work that something outside it produces later, or the helping
    // can nest into a deadlock.
    void wait(WaitGroup& group);

private:
    struct Job {
        std::function<void()> fn;
        WaitGroup* group;
    };

    bool try_run_one();
    void finish(WaitGroup& group);
    void worker_loop();
    void stop_workers();
    void signal(bool all);

    // A submitted job. Posted ones are a list, the newest first, pushed by any thread without
    // a lock; whichever thread next looks for work takes the whole list (under m_mutex) and
    // turns it round: the queue, the oldest first, through the same `next`. Made by submit
    // and freed by the thread that runs it, outside the lock.
    struct Posted {
        Job job;
        Posted* next;
    };
    std::atomic<Posted*> m_posted{nullptr};
    // Counts every job posted and every group finished; a thread with nothing to do sleeps
    // until it changes from what it read before it looked.
    std::atomic<u32> m_signal{0};
    std::atomic<bool> m_stop{false};
    // Protects the queue: taken by the threads that run jobs (the workers, and a thread in
    // wait or parallel_for), for a few pointers' work; never to submit one.
    std::mutex m_mutex;
    Posted* m_first = nullptr; // the queue: the oldest taken in and not yet run
    std::vector<std::thread> m_workers;
};

} // namespace giant
