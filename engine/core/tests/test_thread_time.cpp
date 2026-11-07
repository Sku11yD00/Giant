#include "giant/core/thread_time.h"

#include "giant_test.h"

#include <chrono>
#include <thread>

using namespace giant;

GIANT_TEST("thread time: counts work, not sleep") {
    using clock = std::chrono::steady_clock;
    const double before = thread_cpu_ms();
    // Busy for 30 ms of wall time: most of it is CPU time.
    const auto start = clock::now();
    volatile unsigned long long sink = 0;
    while (clock::now() - start < std::chrono::milliseconds(30)) {
        sink = sink + 1;
    }
    const double busy = thread_cpu_ms() - before;
    GIANT_CHECK(busy > 5.0);
    GIANT_CHECK(busy < 200.0);
    // Asleep for 50 ms: hardly any.
    const double asleep_from = thread_cpu_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const double asleep = thread_cpu_ms() - asleep_from;
    GIANT_CHECK(asleep >= 0.0);
    GIANT_CHECK(asleep < 20.0);
}
