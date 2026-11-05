#include "giant/core/assert.h"
#include "giant/core/log.h"

#include "giant_test.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace giant;

namespace {
int g_assert_count = 0;
void counting_handler(const char*, const char*, const char*, int) { ++g_assert_count; }
} // namespace

GIANT_TEST("assert: a custom handler sees failed asserts only") {
    set_assert_handler(&counting_handler);
    g_assert_count = 0;
    // Runtime values, so MSVC does not warn about constant conditions.
    const int two = g_assert_count + 2;
    GIANT_ASSERT(two == 2, "maths works");
    GIANT_CHECK(g_assert_count == 0);
    GIANT_ASSERT(two == 3, "maths is broken");
    GIANT_CHECK(g_assert_count == 1);
    set_assert_handler(nullptr);
    GIANT_CHECK(assert_handler() == &default_assert_handler);
}

GIANT_TEST("log: sinks receive records at or above their level") {
    set_console_logging(false);
    std::vector<std::string> seen;
    const LogSinkId id = add_log_sink(
        [&seen](const LogRecord& r) {
            seen.push_back(std::string(r.channel) + ":" + std::string(r.text));
        },
        LogLevel::warning);

    log_info("streaming", "cell loaded");
    log_warning("streaming", "cell late");
    log_error("giant", "lost footing");
    remove_log_sink(id);
    log_error("giant", "after removal");
    set_console_logging(true);

    GIANT_CHECK(seen.size() == 2);
    GIANT_CHECK(seen.at(0) == "streaming:cell late");
    GIANT_CHECK(seen.at(1) == "giant:lost footing");
}

GIANT_TEST("log: many threads logging at once deliver every record") {
    set_console_logging(false);
    std::atomic<int> count{0};
    const LogSinkId id = add_log_sink([&count](const LogRecord&) { count.fetch_add(1); });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < 1000; ++i) {
                log_info("test", "tick");
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    remove_log_sink(id);
    set_console_logging(true);
    GIANT_CHECK(count.load() == 4000);
}

// Review finding: a sink that logged (e.g. a failed assert) deadlocked.
GIANT_TEST("log: logging from inside a sink does not deadlock") {
    set_console_logging(false);
    int outer = 0;
    const LogSinkId id = add_log_sink([&outer](const LogRecord& r) {
        if (r.channel == "outer") {
            ++outer;
            log_info("inner", "from inside a sink");
        }
    });
    log_info("outer", "hello");
    remove_log_sink(id);
    set_console_logging(true);
    GIANT_CHECK(outer == 1);
}

// Cross-check finding: with a handler that returns, a sink changing the sink
// list used to deadlock on the log mutex.
GIANT_TEST("log: a sink changing sinks asserts instead of deadlocking") {
    set_assert_handler(&counting_handler);
    g_assert_count = 0;
    set_console_logging(false);
    const LogSinkId id = add_log_sink([](const LogRecord& r) {
        if (r.channel == "meddle") {
            const LogSinkId inner = add_log_sink([](const LogRecord&) {});
            remove_log_sink(inner);
            set_console_logging(true);
        }
    });
    log_info("meddle", "go");
    remove_log_sink(id);
    set_console_logging(true);
    set_assert_handler(nullptr);
    GIANT_CHECK(g_assert_count == 3);
}
