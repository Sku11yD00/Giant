#include "giant_test.h"

#include <atomic>
#include <cstring>

namespace giant::test {

namespace {
// Atomic so checks may run inside jobs on other threads.
std::atomic<int> g_failures_in_current_test{0};
}

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* file, int line, const char* expression) {
    ++g_failures_in_current_test;
    std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, expression);
}

} // namespace giant::test

int main(int argc, char** argv) {
    using namespace giant::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int run = 0;
    int failed = 0;
    for (const TestCase& test : registry()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }
        g_failures_in_current_test.store(0);
        test.fn();
        ++run;
        if (g_failures_in_current_test.load() > 0) {
            ++failed;
            std::fprintf(stderr, "[FAIL] %s (%s:%d)\n", test.name, test.file, test.line);
        } else {
            std::printf("[ ok ] %s\n", test.name);
        }
    }

    std::printf("%d tests, %d failed\n", run, failed);
    if (run == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}
