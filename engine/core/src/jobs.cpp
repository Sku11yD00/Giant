#include "giant/core/jobs.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>

namespace giant {

JobSystem::JobSystem(u32 worker_count) {
    if (worker_count == 0) {
        const u32 hardware = std::thread::hardware_concurrency();
        worker_count = hardware > 1 ? hardware - 1 : 1;
    }
    m_workers.reserve(worker_count);
    for (u32 i = 0; i < worker_count; ++i) {
        m_workers.emplace_back([this](std::stop_token stop) { worker_loop(stop); });
    }
}

JobSystem::~JobSystem() {
    // Finish everything already queued so no WaitGroup is left pending. Jobs
    // that are still running may queue more; workers drain those before exiting.
    while (try_run_one()) {
    }
    for (std::jthread& worker : m_workers) {
        worker.request_stop();
    }
    m_wake.notify_all();
    m_workers.clear(); // joins
}

void JobSystem::submit(WaitGroup& group, std::function<void()> job) {
    GIANT_ASSERT(job != nullptr, "submitted an empty job");
    group.m_pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back({std::move(job), &group});
    }
    m_wake.notify_one();
}

void JobSystem::finish(WaitGroup& group) {
    if (group.m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // The group may be destroyed from here on; only touch the JobSystem.
        // Taking the lock orders this with a waiter checking its predicate,
        // so the wake-up cannot be lost.
        { std::lock_guard lock(m_mutex); }
        m_wake.notify_all();
    }
}

bool JobSystem::try_run_one() {
    std::function<void()> fn;
    WaitGroup* group = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        fn.swap(m_queue.front().fn);
        group = m_queue.front().group;
        m_queue.pop_front();
    }
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

void JobSystem::worker_loop(std::stop_token stop) {
    for (;;) {
        if (try_run_one()) {
            continue;
        }
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, stop, [this] { return !m_queue.empty(); });
        if (stop.stop_requested() && m_queue.empty()) {
            return;
        }
    }
}

void JobSystem::wait(WaitGroup& group) {
    while (!group.done()) {
        if (try_run_one()) {
            continue;
        }
        // Nothing to help with: sleep until a job is queued or a group finishes.
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this, &group] { return group.done() || !m_queue.empty(); });
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
