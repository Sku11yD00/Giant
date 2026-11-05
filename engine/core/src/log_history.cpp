#include "giant/core/log_history.h"

#include <algorithm>

namespace giant {

LogHistory::LogHistory(usize capacity, LogLevel min_level) : m_shared(std::make_shared<Shared>()) {
    m_shared->capacity = std::max<usize>(capacity, 1);
    // The sink holds its own reference, so a record that is being delivered
    // while this object is destroyed never touches freed memory.
    m_sink = add_log_sink(
        [shared = m_shared](const LogRecord& record) {
            std::lock_guard lock(shared->mutex);
            if (shared->records.size() == shared->capacity) {
                shared->records.pop_front();
            }
            shared->records.push_back({shared->next_sequence++, record.level,
                                       std::string(record.channel), std::string(record.text)});
        },
        min_level);
}

LogHistory::~LogHistory() { remove_log_sink(m_sink); }

std::vector<StoredLogRecord> LogHistory::recent(usize count, LogLevel min_level) const {
    std::vector<StoredLogRecord> out;
    std::lock_guard lock(m_shared->mutex);
    for (auto it = m_shared->records.rbegin(); it != m_shared->records.rend() && out.size() < count;
         ++it) {
        if (it->level >= min_level) {
            out.push_back(*it);
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace giant
