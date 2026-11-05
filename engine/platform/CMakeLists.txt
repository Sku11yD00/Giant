giant_module(platform
  SOURCES
    src/window_sdl.cpp
  DEPENDS core
)
target_link_libraries(giant_platform PRIVATE SDL3::SDL3-static)

giant_module_tests(platform
  SOURCES
    tests/test_window.cpp
)
# Tests never need a real display.
set_tests_properties(platform PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
