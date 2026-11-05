// Keeps the most recent log records in memory, so the editor's log panel and
// the AI layer can read what a person would see in the console.
#pragma once

#include "giant/core/log.h"
#include "giant/core/types.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace giant {

struct StoredLogRecord {
    u64 sequence; // increases by one per record, never reused
    LogLevel level;
    std::string channel;
    std::string text;
};

class LogHistory {
public:
    // Starts recording every record at or above `min_level`.
    explicit LogHistory(usize capacity = 2000, LogLevel min_level = LogLevel::trace);
    ~LogHistory();

    LogHistory(const LogHistory&) = delete;
    LogHistory& operator=(const LogHistory&) = delete;

    // The newest `count` records at or above `min_level`, oldest first.
    std::vector<StoredLogRecord> recent(usize count, LogLevel min_level = LogLevel::trace) const;

private:
    struct Shared {
        std::mutex mutex; // protects everything below
        std::deque<StoredLogRecord> records;
        u64 next_sequence = 1;
        usize capacity = 0;
    };
    std::shared_ptr<Shared> m_shared;
    LogSinkId m_sink = 0;
};

} // namespace giant
