giant_module(world
  SOURCES
    src/scene.cpp
    src/terrain.cpp
    src/far_ground.cpp
  src/terrain_brush.cpp
  src/height_map.cpp
  src/scatter.cpp
  DEPENDS core
)
# JSON stays private: the scene API is plain C++ types.
target_link_libraries(giant_world PRIVATE nlohmann_json::nlohmann_json)

giant_module_tests(world
  SOURCES
    tests/test_scene.cpp
    tests/test_terrain.cpp
    tests/test_scatter.cpp
)
