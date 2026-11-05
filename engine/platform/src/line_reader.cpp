#include "giant/platform/line_reader.h"

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
#include <unistd.h>
#endif

#include <chrono>
#include <condition_variable>
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

// Raw reads of standard input, split into lines. Returns false at the end of
// input or on an error.
class StdinLines {
public:
    bool next(std::string& line) {
        for (;;) {
            if (const auto end = m_buffer.find('\n'); end != std::string::npos) {
                line.assign(m_buffer, 0, end);
                m_buffer.erase(0, end + 1);
                return true;
            }
            char chunk[4096];
            const long got = read_chunk(chunk, sizeof(chunk));
            if (got <= 0) {
                if (m_buffer.empty()) {
                    return false;
                }
                line = std::move(m_buffer); // last line without a newline
                m_buffer.clear();
                return true;
            }
            m_buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }

private:
    static long read_chunk(char* data, std::size_t size) {
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), data, static_cast<DWORD>(size), &got,
                      nullptr)) {
            return -1; // includes a closed pipe
        }
        return static_cast<long>(got);
#else
        for (;;) {
            const ssize_t got = ::read(STDIN_FILENO, data, size);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return static_cast<long>(got);
        }
#endif
    }

    std::string m_buffer;
};

} // namespace

LineReader::LineReader(ConstructKey) : m_shared(std::make_shared<Shared>()) {}

std::unique_ptr<LineReader> LineReader::from_stdin() {
    auto reader = std::make_unique<LineReader>(ConstructKey{});
    reader->start([lines = std::make_shared<StdinLines>()](std::string& line) {
        return lines->next(line);
    });
    return reader;
}

LineReader::LineReader(std::istream& in) : m_shared(std::make_shared<Shared>()) {
    start([&in](std::string& line) { return static_cast<bool>(std::getline(in, line)); });
}

void LineReader::start(std::function<bool(std::string&)> read_line) {
    // The thread holds its own reference to the shared state, so it stays
    // valid even if the reader is destroyed while the thread is blocked.
    m_shared->thread = std::thread([shared = m_shared, read_line = std::move(read_line)] {
        std::string line;
        while (read_line(line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back(); // Windows line endings
            }
            std::lock_guard lock(shared->mutex);
            shared->lines.push_back(std::move(line));
            shared->changed.notify_all();
        }
        std::lock_guard lock(shared->mutex);
        shared->ended = true;
        shared->changed.notify_all();
    });
}

LineReader::~LineReader() {
    bool ended = false;
    {
        std::lock_guard lock(m_shared->mutex);
        ended = m_shared->ended;
    }
    if (ended) {
        m_shared->thread.join();
    } else {
        // Still blocked in a read that may never return (stdin of a live
        // process). The thread owns its state, so letting it go is safe; the
        // process exit ends it.
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
