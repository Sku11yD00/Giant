#include "giant/core/jobs.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/core/thread_time.h"

#include <algorithm>

namespace giant {

JobSystem::JobSystem(u32 worker_count) {
    if (worker_count == 0) {
        const u32 hardware = std::thread::hardware_concurrency();
        worker_count = hardware > 1 ? hardware - 1 : 1;
    }
    m_workers.reserve(worker_count);
    try {
        for (u32 i = 0; i < worker_count; ++i) {
            m_workers.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        stop_workers(); // (a thread the system would not give: those started are joined)
        throw;
    }
}

JobSystem::~JobSystem() {
    // Finish everything already queued so no WaitGroup is left pending. Jobs
    // that are still running may queue more; workers drain those before exiting.
    while (try_run_one()) {
    }
    stop_workers();
    // (A job some other thread submitted as the workers left: run here, not dropped.)
    while (try_run_one()) {
    }
}

void JobSystem::stop_workers() {
    m_stop.store(true);
    signal(true);
    for (std::thread& worker : m_workers) {
        worker.join();
    }
    m_workers.clear();
}

void JobSystem::signal(bool all) {
    m_signal.fetch_add(1);
    if (all) {
        m_signal.notify_all();
    } else {
        m_signal.notify_one();
    }
}

void JobSystem::submit(WaitGroup& group, std::function<void()> job) {
    GIANT_ASSERT(job != nullptr, "submitted an empty job");
    // (The one `new` here: a node for the list, freed by the thread that runs the job.)
    Posted* const posted = new Posted{{std::move(job), &group}, nullptr};
    group.m_pending.fetch_add(1, std::memory_order_relaxed);
    posted->next = m_posted.load(std::memory_order_relaxed);
    while (!m_posted.compare_exchange_weak(posted->next, posted, std::memory_order_release, std::memory_order_relaxed)) {
    }
    signal(false);
}

void JobSystem::finish(WaitGroup& group) {
    if (group.m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // The group may be destroyed from here on; only touch the JobSystem. A waiter read
        // the signal before it looked at its group, so it either saw the group done or wakes.
        signal(true);
    }
}

bool JobSystem::try_run_one() {
    Posted* taken = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (m_first == nullptr && m_posted.load(std::memory_order_acquire) != nullptr) {
            // All that was posted, turned round into the order it was posted in.
            Posted* posted = m_posted.exchange(nullptr, std::memory_order_acquire);
            while (posted != nullptr) {
                Posted* const next = posted->next;
                posted->next = m_first;
                m_first = posted;
                posted = next;
            }
        }
        if (m_first == nullptr) {
            return false;
        }
        taken = m_first;
        m_first = taken->next;
    }
    std::function<void()> fn = std::move(taken->job.fn);
    WaitGroup* const group = taken->job.group;
    delete taken;
    try {
        fn();
    } catch (...) {
        log_error("jobs", "a job threw an exception");
        GIANT_ASSERT(false, "jobs must not throw");
    }
    // Destroy the job and its captures before the group is released, so
    // nothing the job owned outlives wait().
    fn = nullptr;
    finish(*group);
    return true;
}

void JobSystem::worker_loop() {
    // (A thread woken from a wait is boosted for a moment on Windows: not these, or with every
    // core busy the one just woken takes the core of the thread that woke it, the frame's.)
    no_wake_boost_for_this_thread();
    for (;;) {
        // The signal is read before looking: a job posted after the look changes it, and the
        // wait returns at once. (A look that finds nothing found nothing posted either.)
        const u32 seen = m_signal.load();
        if (try_run_one()) {
            continue;
        }
        if (m_stop.load()) {
            return;
        }
        m_signal.wait(seen);
    }
}

void JobSystem::wait(WaitGroup& group) {
    while (!group.done()) {
        const u32 seen = m_signal.load();
        if (try_run_one()) {
            continue;
        }
        // Nothing to help with: sleep until a job is posted or a group finishes.
        if (group.done()) {
            break;
        }
        m_signal.wait(seen);
    }
}

void JobSystem::parallel_for(usize count, usize batch_size,
                             const std::function<void(usize begin, usize end)>& fn) {
    if (count == 0) {
        return;
    }
    batch_size = std::max<usize>(batch_size, 1);
    WaitGroup group;
    for (usize begin = 0; begin < count; begin += batch_size) {
        const usize end = std::min(count, begin + batch_size);
        submit(group, [&fn, begin, end] { fn(begin, end); });
    }
    wait(group);
}

} // namespace giant
