#include "giant/core/log_history.h"

#include "giant_test.h"

using namespace giant;

GIANT_TEST("log history: keeps the newest records, oldest first") {
    set_console_logging(false);
    LogHistory history(3);
    log_info("a", "one");
    log_info("a", "two");
    log_warning("b", "three");
    log_error("b", "four");
    const auto all = history.recent(10);
    GIANT_CHECK(all.size() == 3);
    if (all.size() == 3) {
        GIANT_CHECK(all[0].text == "two");
        GIANT_CHECK(all[2].text == "four");
        GIANT_CHECK(all[2].channel == "b");
        GIANT_CHECK(all[1].sequence + 1 == all[2].sequence);
    }
    const auto last = history.recent(1);
    GIANT_CHECK(last.size() == 1 && last[0].text == "four");
    set_console_logging(true);
}

GIANT_TEST("log history: filters by level") {
    set_console_logging(false);
    LogHistory history(10);
    log_info("a", "fine");
    log_warning("a", "hmm");
    log_error("a", "bad");
    const auto warnings = history.recent(10, LogLevel::warning);
    GIANT_CHECK(warnings.size() == 2);
    GIANT_CHECK(history.recent(10, LogLevel::error).size() == 1);
    set_console_logging(true);
}

GIANT_TEST("log history: stops recording once destroyed") {
    set_console_logging(false);
    {
        LogHistory history(10);
        log_info("a", "seen");
        GIANT_CHECK(history.recent(10).size() == 1);
    }
    log_info("a", "after"); // must not touch the destroyed history
    set_console_logging(true);
}
