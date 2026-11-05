#include "giant/core/assert.h"

#include "giant/core/log.h"

#include <atomic>
#include <cstdlib>

namespace giant {

namespace {
// Protected by being atomic: may be swapped while other threads assert.
std::atomic<AssertHandler> g_handler{&default_assert_handler};
} // namespace

void set_assert_handler(AssertHandler handler) {
    g_handler.store(handler != nullptr ? handler : &default_assert_handler);
}

AssertHandler assert_handler() { return g_handler.load(); }

void default_assert_handler(const char* expression, const char* message, const char* file,
                            int line) {
    log_message(LogLevel::error, "assert",
                std::string("assert failed: ") + expression + " (" + message + ") at " + file +
                    ":" + std::to_string(line));
    std::abort();
}

namespace detail {
void assert_failed(const char* expression, const char* message, const char* file, int line) {
    g_handler.load()(expression, message, file, line);
}
} // namespace detail

} // namespace giant
