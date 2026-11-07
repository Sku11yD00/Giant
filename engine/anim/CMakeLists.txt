giant_module(anim
  SOURCES
    src/skeleton.cpp
    src/player.cpp
  DEPENDS core
)

giant_module_tests(anim
  SOURCES
    tests/test_anim.cpp
)
