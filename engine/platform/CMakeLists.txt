giant_module(platform
  SOURCES
    src/line_reader.cpp
    src/window_sdl.cpp
  DEPENDS core
)
target_link_libraries(giant_platform PRIVATE SDL3::SDL3-static)

giant_module_tests(platform
  SOURCES
    tests/test_line_reader.cpp
    tests/test_window.cpp
)
# The tests post synthetic OS events, so they see SDL directly.
if(GIANT_BUILD_TESTS)
  target_link_libraries(giant_platform_tests PRIVATE SDL3::SDL3-static)
  # Tests never need a real display.
  set_tests_properties(platform PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
