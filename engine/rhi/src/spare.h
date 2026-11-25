// Private: one value kept made ahead of need, on a thread of its own.
//
// Some things a device makes are slow to make and wanted without warning: a
// 64 MiB heap for small buffers is new video memory, found and mapped by the
// OS (and zeroed, on runtimes that cannot skip it), and making it in the frame
// whose buffer found the heaps full held that frame for milliseconds. A Spare
// makes the next one as soon as the last is taken, off the caller's thread;
// the caller takes it when it is there and does without when it is not.
//
// No GPU in it, so it is tested on every platform.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/types.h"

#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace giant::rhi {

template <class T>
class Spare {
public:
    // Makes one, on the Spare's thread (or, in wait() and on destruction, the
    // caller's); nothing when it cannot be made, or it throws (no more are
    // tried then).
    using Make = std::function<std::optional<T>()>;

    explicit Spare(Make make) : m_make(std::move(make)) {}
    // Waits for the one being made.
    ~Spare() { m_jobs.wait(m_group); }

    Spare(const Spare&) = delete;
    Spare& operator=(const Spare&) = delete;

    // Starts making one, unless one is ready, one is being made, or making
    // failed. Never waits.
    void want() {
        {
            const std::lock_guard lock(m_mutex);
            if (m_ready || m_making || m_failed) {
                return;
            }
            m_making = true;
        }
        m_jobs.submit(m_group, [this] {
            std::optional<T> made;
            try {
                made = m_make();
            } catch (const std::exception&) {
                made.reset(); // (no exceptions leave the job: one that throws could not be made)
            }
            const std::lock_guard lock(m_mutex);
            m_making = false;
            m_failed = !made.has_value();
            m_ready = std::move(made);
        });
    }

    // The one ready, which is then the caller's; nothing while none is. Never
    // waits.
    std::optional<T> take() {
        const std::lock_guard lock(m_mutex);
        std::optional<T> out = std::move(m_ready);
        m_ready.reset();
        return out;
    }

    // Whether one is being made now.
    bool making() const {
        const std::lock_guard lock(m_mutex);
        return m_making;
    }
    // Whether making one failed (none is made again).
    bool failed() const {
        const std::lock_guard lock(m_mutex);
        return m_failed;
    }
    // Blocks until none is being made (start-up, tests; never in a frame).
    void wait() { m_jobs.wait(m_group); }

private:
    Make m_make;
    mutable std::mutex m_mutex; // protects the three below
    std::optional<T> m_ready;
    bool m_making = false;
    bool m_failed = false;
    WaitGroup m_group;
    JobSystem m_jobs{1}; // last: destroyed (its job done) first
};

} // namespace giant::rhi
