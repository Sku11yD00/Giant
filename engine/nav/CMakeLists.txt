giant_module(nav
  SOURCES
    src/nav.cpp
    src/follow.cpp
  DEPENDS core world
)

giant_module_tests(nav
  SOURCES
    tests/test_nav.cpp
    tests/test_follow.cpp
)
