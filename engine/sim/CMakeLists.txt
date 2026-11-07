giant_module(sim
  SOURCES
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
    tests/test_physics.cpp
    tests/test_scene_physics.cpp
)
