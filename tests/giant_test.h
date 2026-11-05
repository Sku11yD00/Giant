// Minimal unit-test harness for Giant Engine.
//
//   GIANT_TEST("math: camera relative keeps millimetres at 1000 km") {
//       GIANT_CHECK(value == expected);
//       GIANT_CHECK_NEAR(a, b, 1e-6);
//   }
//
// Each module builds one test executable linked with giant::test_main.
// Run one test by passing a substring of its name as the first argument.
#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace giant::test {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
    const char* file;
    int line;
};

std::vector<TestCase>& registry();
void report_failure(const char* file, int line, const char* expression);

struct Registrar {
    Registrar(const char* name, TestFn fn, const char* file, int line) {
        registry().push_back({name, fn, file, line});
    }
};

} // namespace giant::test

#define GIANT_TEST_CONCAT_INNER(a, b) a##b
#define GIANT_TEST_CONCAT(a, b) GIANT_TEST_CONCAT_INNER(a, b)

#define GIANT_TEST_IMPL(name, fn)                                                                   \
    static void fn();                                                                               \
    static const ::giant::test::Registrar GIANT_TEST_CONCAT(fn, _registrar){name, &fn, __FILE__,    \
                                                                            __LINE__};              \
    static void fn()

#define GIANT_TEST(name) GIANT_TEST_IMPL(name, GIANT_TEST_CONCAT(giant_test_fn_, __COUNTER__))

#define GIANT_CHECK(expr)                                                                           \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            ::giant::test::report_failure(__FILE__, __LINE__, #expr);                               \
        }                                                                                           \
    } while (false)

#define GIANT_CHECK_NEAR(a, b, tolerance)                                                           \
    do {                                                                                            \
        const double giant_check_a = static_cast<double>(a);                                        \
        const double giant_check_b = static_cast<double>(b);                                        \
        if (!(std::fabs(giant_check_a - giant_check_b) <= static_cast<double>(tolerance))) {        \
            std::fprintf(stderr, "    %s = %.9g, %s = %.9g\n", #a, giant_check_a, #b,               \
                         giant_check_b);                                                            \
            ::giant::test::report_failure(__FILE__, __LINE__, #a " ~= " #b);                        \
        }                                                                                           \
    } while (false)
