#include "giant/platform/line_writer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>

namespace giant::platform {

struct LineWriter::Shared {
    std::mutex mutex; // protects all below but thread
    std::condition_variable changed;
    std::deque<std::string> lines; // sent, not yet taken by the thread
    usize waiting = 0;             // bytes of `lines` and of the write under way
    bool failed = false;
    bool leaving = false; // the writer is going: stop once nothing waits
    bool ended = false;   // the thread is done
    std::thread thread;
};

namespace {

// The writer thread: one line at a time, so nothing is held while it blocks.
void write_lines(LineWriter::Shared& shared, const LineWriter::WriteAll& write) {
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(shared.mutex);
            shared.changed.wait(lock, [&] { return !shared.lines.empty() || shared.leaving; });
            if (shared.lines.empty()) {
                return;
            }
            line = std::move(shared.lines.front());
            shared.lines.pop_front();
        }
        // In pieces, so what waits is seen to fall while a long line is being read.
        constexpr usize k_piece = 64u * 1024;
        std::string_view rest = line;
        bool written = true;
        while (written && !rest.empty()) {
            const std::string_view piece = rest.substr(0, k_piece);
            try {
                written = write(piece);
            } catch (const std::exception& e) {
                // No exceptions leave the thread: a sink that throws is one that is gone.
                log_error("platform", std::string("writing output failed: ") + e.what());
                written = false;
            }
            if (written) {
                rest.remove_prefix(piece.size());
                std::lock_guard lock(shared.mutex);
                shared.waiting -= piece.size();
                shared.changed.notify_all();
            }
        }
        if (!written) {
            std::lock_guard lock(shared.mutex);
            shared.failed = true;
            shared.lines.clear();
            shared.waiting = 0;
            shared.changed.notify_all();
            return;
        }
    }
}

#if defined(_WIN32)
bool write_stdout(std::string_view text) {
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    while (!text.empty()) {
        const DWORD size = static_cast<DWORD>(std::min<usize>(text.size(), 1u << 20));
        DWORD put = 0;
        if (!WriteFile(handle, text.data(), size, &put, nullptr)) {
            return false; // ERROR_BROKEN_PIPE, ERROR_NO_DATA: the client closed its end
        }
        if (put == 0) {
            // A pipe handed over in no-wait mode, full: wait for room instead of spinning.
            Sleep(1);
        }
        text.remove_prefix(put);
    }
    return true;
}
#else
bool write_stdout(std::string_view text) {
    while (!text.empty()) {
        const ssize_t put = ::write(STDOUT_FILENO, text.data(), text.size());
        if (put >= 0) {
            text.remove_prefix(static_cast<usize>(put));
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Handed a non-blocking stdout: wait for room instead of giving up.
            pollfd fd{STDOUT_FILENO, POLLOUT, 0};
            ::poll(&fd, 1, -1);
            continue;
        }
        return false; // EPIPE: the client closed its end
    }
    return true;
}
#endif

} // namespace

std::unique_ptr<LineWriter> LineWriter::to_stdout() {
    static std::atomic<bool> created{false};
    const bool first = !created.exchange(true);
    GIANT_ASSERT(first, "LineWriter::to_stdout may only be called once per process");
    return std::make_unique<LineWriter>(&write_stdout);
}

LineWriter::LineWriter(WriteAll write) : m_shared(std::make_shared<Shared>()) {
    try {
        // The thread holds its own reference to the shared state, so it stays
        // valid even if the writer is destroyed while the thread is blocked.
        m_shared->thread = std::thread([shared = m_shared, write = std::move(write)] {
            write_lines(*shared, write);
            std::lock_guard lock(shared->mutex);
            shared->ended = true;
            shared->changed.notify_all();
        });
    } catch (const std::exception& e) {
        // No exceptions leave the module: report it as a sink that is gone.
        log_error("platform", std::string("could not start the output thread: ") + e.what());
        std::lock_guard lock(m_shared->mutex);
        m_shared->failed = true;
        m_shared->ended = true;
    }
}

LineWriter::~LineWriter() {
    if (!m_shared->thread.joinable()) {
        return;
    }
    bool idle = false;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->leaving = true;
        // Nothing waits and none is being written, or the thread is done: it ends at once.
        idle = m_shared->ended || m_shared->waiting == 0;
        if (!idle) {
            m_shared->lines.clear(); // (what waits behind the write under way is never written)
        }
        m_shared->changed.notify_all();
    }
    if (idle) {
        m_shared->thread.join();
    } else {
        // Blocked in a write that may never return (a client that stopped
        // reading). The thread owns its state, so letting it go is safe; it
        // ends with the write or with the process.
        m_shared->thread.detach();
    }
}

bool LineWriter::send(std::string line) {
    line += '\n';
    std::lock_guard lock(m_shared->mutex);
    if (m_shared->failed) {
        return false;
    }
    m_shared->waiting += line.size();
    m_shared->lines.push_back(std::move(line));
    m_shared->changed.notify_all();
    return true;
}

bool LineWriter::ok() const {
    std::lock_guard lock(m_shared->mutex);
    return !m_shared->failed;
}

usize LineWriter::waiting_bytes() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->waiting;
}

bool LineWriter::finish(int timeout_ms) const {
    std::unique_lock lock(m_shared->mutex);
    m_shared->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return m_shared->failed || m_shared->waiting == 0; });
    return !m_shared->failed && m_shared->waiting == 0;
}

} // namespace giant::platform
