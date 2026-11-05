// Asserts for programmer mistakes. Expected failures (missing files, bad data)
// must return a result instead; see docs/coding-rules.md.
#pragma once

namespace giant {

// Called when an assert fails. The default handler logs and aborts.
// Tests and the editor can install their own handler.
using AssertHandler = void (*)(const char* expression, const char* message, const char* file,
                               int line);

void set_assert_handler(AssertHandler handler);
AssertHandler assert_handler();

[[noreturn]] void default_assert_handler(const char* expression, const char* message,
                                         const char* file, int line);

namespace detail {
void assert_failed(const char* expression, const char* message, const char* file, int line);
}

} // namespace giant

// Checked in every build type: the engine favours catching bad state early.
#define GIANT_ASSERT(expr, message)                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            ::giant::detail::assert_failed(#expr, message, __FILE__, __LINE__);                     \
        }                                                                                           \
    } while (false)
