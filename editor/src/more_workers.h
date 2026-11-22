#pragma once

// Threads that work beside a system's own only while they are asked for: a
// game's loading screen, where the world is to come in as fast as the
// machine can and a frame may take what it takes. In play they are given
// nothing (they sleep once what a loading screen left them is done), so
// the frame keeps its cores. Used by the main thread only.

#include "giant/core/jobs.h"
#include "giant/core/types.h"

#include <algorithm>
#include <memory>
#include <thread>

namespace giant::editor {

class MoreWorkers {
public:
    static constexpr u32 k_most = 6;
    // How many a loading screen asks each system for: a quarter of the cores
    // (the cells' readers, the ground's builders and the plants' placers
    // work at once), one to k_most.
    static u32 for_loading() { return std::clamp(std::thread::hardware_concurrency() / 4, 1u, k_most); }

    // Whose turn the next job is, of `own` threads of the system's and `more`
    // of these (at most k_most; started the first time they are asked for):
    // these threads' pool, to submit it to with group(), or null when it is
    // the system's own (always, while `more` is 0).
    JobSystem* next(u32 more, u32 own) {
        more = std::min(more, k_most);
        if (more == 0)
            return nullptr;
        if (m_jobs == nullptr || m_jobs->worker_count() < more) {
            wait();
            m_jobs = std::make_unique<JobSystem>(more);
        }
        const bool mine = m_turn++ % (u64{own} + more) >= own;
        m_handed += mine ? 1u : 0u;
        return mine ? m_jobs.get() : nullptr;
    }
    WaitGroup& group() { return m_group; }
    // Until every job handed to these threads is done.
    void wait() {
        if (m_jobs != nullptr)
            m_jobs->wait(m_group);
    }
    u64 handed() const { return m_handed; } // jobs given to these threads, ever (tests)

private:
    WaitGroup m_group;
    u64 m_turn = 0;
    u64 m_handed = 0;
    std::unique_ptr<JobSystem> m_jobs;
};

} // namespace giant::editor
