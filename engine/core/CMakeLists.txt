giant_module(core
  SOURCES
    src/assert.cpp
    src/jobs.cpp
    src/log.cpp
    src/log_history.cpp
    src/math.cpp
    src/image.cpp
    src/png.cpp
    src/thread_time.cpp
    src/wind.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(giant_core PUBLIC Threads::Threads)

giant_module_tests(core
  SOURCES
    tests/test_assert_log.cpp
    tests/test_image.cpp
    tests/test_jobs.cpp
    tests/test_log_history.cpp
    tests/test_math.cpp
    tests/test_png.cpp
    tests/test_thread_time.cpp
    tests/test_wind.cpp
)
