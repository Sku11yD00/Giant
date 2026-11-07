#include "giant/core/thread_time.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <chrono>
#else
#include <time.h>
#endif

namespace giant {

#if defined(_WIN32)
namespace {

// QueryThreadCycleTime counts the processor's time stamp counter while the
// thread runs: cycles per millisecond, measured once against the clock.
double cycles_per_ms() {
    static const double rate = [] {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        const unsigned long long first = __rdtsc();
        while (clock::now() - start < std::chrono::milliseconds(5)) {
        }
        const unsigned long long last = __rdtsc();
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
        return ms > 0.0 && last > first ? static_cast<double>(last - first) / ms : 1e6;
    }();
    return rate;
}

} // namespace

double thread_cpu_ms() {
    const double rate = cycles_per_ms();
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        return 0.0;
    }
    return static_cast<double>(cycles) / rate;
}
#else
double thread_cpu_ms() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) {
        return 0.0;
    }
    return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_nsec) / 1e6;
}
#endif

} // namespace giant
