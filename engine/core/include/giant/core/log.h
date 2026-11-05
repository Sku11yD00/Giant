// Thread-safe logging with pluggable sinks, so the editor and the AI layer can
// read the same log a person sees.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace giant {

enum class LogLevel { trace, info, warning, error };

const char* to_string(LogLevel level);

struct LogRecord {
    LogLevel level;
    std::string_view channel; // e.g. "render", "streaming", "giant"
    std::string_view text;
};

using LogSink = std::function<void(const LogRecord&)>;
using LogSinkId = int;

// Adds a sink that receives every record at or above `min_level`.
LogSinkId add_log_sink(LogSink sink, LogLevel min_level = LogLevel::trace);
void remove_log_sink(LogSinkId id);

// Turns the built-in stderr sink on or off (on by default).
void set_console_logging(bool enabled);

void log_message(LogLevel level, std::string_view channel, std::string_view text);

inline void log_info(std::string_view channel, std::string_view text) {
    log_message(LogLevel::info, channel, text);
}
inline void log_warning(std::string_view channel, std::string_view text) {
    log_message(LogLevel::warning, channel, text);
}
inline void log_error(std::string_view channel, std::string_view text) {
    log_message(LogLevel::error, channel, text);
}

} // namespace giant
