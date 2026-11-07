#include "giant/core/thread_time.h"

#include "giant_test.h"

#include <chrono>
#include <thread>

using namespace giant;

GIANT_TEST("thread time: counts work, not sleep") {
    using clock = std::chrono::steady_clock;
    // Busy until 10 ms of CPU time have passed (on a loaded machine that
    // takes longer than 10 ms of wall time, never less).
    const auto start = clock::now();
    const double before = thread_cpu_ms();
    volatile unsigned long long sink = 0;
    double busy = 0.0;
    while (busy < 10.0 && clock::now() - start < std::chrono::seconds(5)) {
        sink = sink + 1;
        busy = thread_cpu_ms() - before;
    }
    const double wall = std::chrono::duration<double, std::milli>(clock::now() - start).count();
    GIANT_CHECK(busy >= 10.0);
    GIANT_CHECK(busy <= wall * 1.1 + 1.0); // never more CPU than wall time
    // Asleep for 50 ms: hardly any.
    const double asleep_from = thread_cpu_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const double asleep = thread_cpu_ms() - asleep_from;
    GIANT_CHECK(asleep >= 0.0);
    GIANT_CHECK(asleep < 20.0);
}
