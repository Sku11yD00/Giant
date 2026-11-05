#include "giant/core/log.h"

#include "giant/core/assert.h"

#include <cstdio>
#include <mutex>
#include <vector>

namespace giant {

namespace {

struct SinkEntry {
    LogSinkId id;
    LogSink sink;
    LogLevel min_level;
};

struct LogState {
    std::mutex mutex; // protects everything below
    std::vector<SinkEntry> sinks;
    LogSinkId next_id = 1;
    bool console = true;
};

// Deliberately never destroyed (the one allowed `new` outside an allocator):
// jobs and static objects may still log while the program shuts down.
LogState& state() {
    static LogState& s = *new LogState();
    return s;
}

// Set while this thread runs sinks. Logging from inside a sink (for example a
// failed assert) goes straight to stderr instead of deadlocking on the mutex.
thread_local bool t_in_sink = false;

void write_console(LogLevel level, std::string_view channel, std::string_view text) {
    std::fprintf(stderr, "[%s] %.*s: %.*s\n", to_string(level), static_cast<int>(channel.size()),
                 channel.data(), static_cast<int>(text.size()), text.data());
}

} // namespace

const char* to_string(LogLevel level) {
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    }
    return "unknown";
}

LogSinkId add_log_sink(LogSink sink, LogLevel min_level) {
    if (t_in_sink) {
        // Locking here would deadlock on the mutex the sink already holds.
        GIANT_ASSERT(false, "sinks must not add sinks");
        return 0;
    }
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    const LogSinkId id = s.next_id++;
    s.sinks.push_back({id, std::move(sink), min_level});
    return id;
}

void remove_log_sink(LogSinkId id) {
    if (t_in_sink) {
        GIANT_ASSERT(false, "sinks must not remove sinks");
        return;
    }
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [id](const SinkEntry& e) { return e.id == id; });
}

void set_console_logging(bool enabled) {
    if (t_in_sink) {
        GIANT_ASSERT(false, "sinks must not change console logging");
        return;
    }
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.console = enabled;
}

void log_message(LogLevel level, std::string_view channel, std::string_view text) {
    if (t_in_sink) {
        write_console(level, channel, text);
        return;
    }
    LogState& s = state();
    const LogRecord record{level, channel, text};
    // Sinks run under the lock so every sink sees records in the same order.
    std::lock_guard lock(s.mutex);
    if (s.console) {
        write_console(level, channel, text);
    }
    struct InSinkScope {
        InSinkScope() { t_in_sink = true; }
        ~InSinkScope() { t_in_sink = false; }
        InSinkScope(const InSinkScope&) = delete;
        InSinkScope& operator=(const InSinkScope&) = delete;
    } in_sink;
    for (const SinkEntry& entry : s.sinks) {
        if (level >= entry.min_level) {
            entry.sink(record);
        }
    }
}

} // namespace giant
