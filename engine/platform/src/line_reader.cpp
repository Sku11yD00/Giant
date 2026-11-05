#include "giant/platform/line_reader.h"

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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

namespace giant::platform {

struct LineReader::Shared {
    std::mutex mutex; // protects lines and ended
    std::condition_variable changed;
    std::vector<std::string> lines;
    bool ended = false;
    std::thread thread;
};

namespace {

void push_line(LineReader::Shared& shared, std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back(); // Windows line endings
    }
    std::lock_guard lock(shared.mutex);
    shared.lines.push_back(std::move(line));
    shared.changed.notify_all();
}

// The reader thread: splits chunks into lines. Each chunk is only scanned
// once, so long lines stay linear in time; overlong lines are dropped.
void read_lines(LineReader::Shared& shared, const LineReader::ReadChunk& read) {
    std::vector<char> chunk(64 * 1024);
    std::string pending;
    bool dropping = false; // inside a line that was too long
    for (;;) {
        const long got = read(chunk.data(), chunk.size());
        if (got <= 0) {
            break;
        }
        const char* data = chunk.data();
        usize size = static_cast<usize>(got);
        while (size > 0) {
            const auto* newline = static_cast<const char*>(std::memchr(data, '\n', size));
            const usize take = newline != nullptr ? static_cast<usize>(newline - data) : size;
            if (!dropping) {
                if (pending.size() + take > LineReader::k_max_line_bytes) {
                    log_error("platform", "input line longer than 64 MiB dropped");
                    pending.clear();
                    pending.shrink_to_fit();
                    dropping = true;
                } else {
                    pending.append(data, take);
                }
            }
            if (newline == nullptr) {
                break;
            }
            push_line(shared, dropping ? std::string(LineReader::k_too_long_line)
                                       : std::move(pending));
            pending.clear();
            dropping = false;
            data += take + 1;
            size -= take + 1;
        }
    }
    if (!pending.empty() && !dropping) {
        push_line(shared, std::move(pending)); // last line without a newline
    }
}

#if defined(_WIN32)
long read_stdin(char* data, usize size) {
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    static const bool is_pipe = GetFileType(handle) == FILE_TYPE_PIPE;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, data, static_cast<DWORD>(size), &got, nullptr)) {
            if (GetLastError() == ERROR_MORE_DATA) {
                return static_cast<long>(got); // message-mode pipe: the rest follows
            }
            return -1; // ERROR_BROKEN_PIPE: the client closed its end
        }
        if (got == 0 && is_pipe) {
            continue; // a zero-byte write on a pipe, not the end
        }
        return static_cast<long>(got); // 0 here: end of a file or console (Ctrl+Z)
    }
}
#else
long read_stdin(char* data, usize size) {
    for (;;) {
        const ssize_t got = ::read(STDIN_FILENO, data, size);
        if (got >= 0) {
            return static_cast<long>(got);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Handed a non-blocking stdin: wait for data instead of quitting.
            pollfd fd{STDIN_FILENO, POLLIN, 0};
            ::poll(&fd, 1, -1);
            continue;
        }
        return -1;
    }
}
#endif

} // namespace

std::unique_ptr<LineReader> LineReader::from_stdin() {
    static std::atomic<bool> created{false};
    const bool first = !created.exchange(true);
    GIANT_ASSERT(first, "LineReader::from_stdin may only be called once per process");
    return std::make_unique<LineReader>(&read_stdin);
}

LineReader::LineReader(ReadChunk read) : m_shared(std::make_shared<Shared>()) {
    try {
        // The thread holds its own reference to the shared state, so it stays
        // valid even if the reader is destroyed while the thread is blocked.
        m_shared->thread = std::thread([shared = m_shared, read = std::move(read)] {
            read_lines(*shared, read);
            std::lock_guard lock(shared->mutex);
            shared->ended = true;
            shared->changed.notify_all();
        });
    } catch (const std::exception& e) {
        // No exceptions leave the module: report it as input that ended.
        log_error("platform", std::string("could not start the input thread: ") + e.what());
        std::lock_guard lock(m_shared->mutex);
        m_shared->ended = true;
    }
}

LineReader::~LineReader() {
    if (!m_shared->thread.joinable()) {
        return;
    }
    bool ended = false;
    {
        std::lock_guard lock(m_shared->mutex);
        ended = m_shared->ended;
    }
    if (ended) {
        m_shared->thread.join();
    } else {
        // Still blocked in a read that may never return (stdin of a live
        // process). The thread owns its state, so letting it go is safe; it
        // ends with the read or with the process.
        m_shared->thread.detach();
    }
}

std::vector<std::string> LineReader::take_lines() {
    std::lock_guard lock(m_shared->mutex);
    std::vector<std::string> out;
    out.swap(m_shared->lines);
    return out;
}

bool LineReader::finished() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->ended && m_shared->lines.empty();
}

void LineReader::wait(int timeout_ms) const {
    std::unique_lock lock(m_shared->mutex);
    m_shared->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return m_shared->ended || !m_shared->lines.empty(); });
}

} // namespace giant::platform
