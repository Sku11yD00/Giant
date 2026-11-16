giant_module(anim
  SOURCES
    src/skeleton.cpp
    src/player.cpp
    src/motion.cpp
    src/matcher.cpp
  DEPENDS core
)

giant_module_tests(anim
  SOURCES
    tests/test_anim.cpp
    tests/test_motion.cpp
    tests/test_matcher.cpp
)
