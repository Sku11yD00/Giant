giant_module(sim
  SOURCES
    src/cloth.cpp
    src/dust.cpp
    src/forest.cpp
    src/hair.cpp
    src/hit_shapes.cpp
    src/ballistics.cpp
    src/physics.cpp
    src/scene_physics.cpp
    src/swing.cpp
  DEPENDS core world
)
# Jolt stays private: the physics API is plain C++ types.
target_link_libraries(giant_sim PRIVATE third_party::jolt)

giant_module_tests(sim
  SOURCES
    tests/test_breakage.cpp
    tests/test_cloth.cpp
    tests/test_character.cpp
    tests/test_dust.cpp
    tests/test_forest.cpp
    tests/test_hair.cpp
    tests/test_hit_shapes.cpp
    tests/test_ballistics.cpp
    tests/test_physics.cpp
    tests/test_ragdoll.cpp
    tests/test_scene_physics.cpp
    tests/test_swing.cpp
    tests/test_water_physics.cpp
)
if(GIANT_BUILD_TESTS)
  # Jolt in an unoptimized MSVC build is slow: the whole test takes ~200 s on
  # a CI Windows runner and three times that on a slow one (it passed 600 s
  # twice with every case passing, each time on a runner where every other
  # test took two to three times as long too).
  set_tests_properties(sim PROPERTIES TIMEOUT 1800)
endif()
