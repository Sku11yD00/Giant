giant_module(nav
  SOURCES
    src/nav.cpp
  DEPENDS core world
)

giant_module_tests(nav
  SOURCES
    tests/test_nav.cpp
)
