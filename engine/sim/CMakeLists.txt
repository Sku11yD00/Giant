giant_module(sim
  SOURCES
    src/dust.cpp
    src/forest.cpp
    src/hit_shapes.cpp
    src/ballistics.cpp
    src/physics.cpp
    src/scene_physics.cpp
  DEPENDS core world
)
# Jolt stays private: the physics API is plain C++ types.
target_link_libraries(giant_sim PRIVATE third_party::jolt)

giant_module_tests(sim
  SOURCES
    tests/test_breakage.cpp
    tests/test_character.cpp
    tests/test_dust.cpp
    tests/test_forest.cpp
    tests/test_hit_shapes.cpp
    tests/test_ballistics.cpp
    tests/test_physics.cpp
    tests/test_scene_physics.cpp
)
if(GIANT_BUILD_TESTS)
  # Jolt in an unoptimized MSVC build is slow: the character tests alone take
  # ~100 s on a CI Windows runner, past the default 120 s.
  set_tests_properties(sim PROPERTIES TIMEOUT 600)
endif()
