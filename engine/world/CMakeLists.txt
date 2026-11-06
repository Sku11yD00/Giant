giant_module(world
  SOURCES
    src/scene.cpp
    src/terrain.cpp
  DEPENDS core
)
# JSON stays private: the scene API is plain C++ types.
target_link_libraries(giant_world PRIVATE nlohmann_json::nlohmann_json)

giant_module_tests(world
  SOURCES
    tests/test_scene.cpp
    tests/test_terrain.cpp
)
