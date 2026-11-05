giant_module(geometry
  SOURCES
    src/clusters.cpp
    src/gltf.cpp
    src/shapes.cpp
  DEPENDS core
)
# Third-party code stays private: the geometry API is plain C++ types.
target_link_libraries(giant_geometry PRIVATE third_party::meshoptimizer third_party::cgltf)

giant_module_tests(geometry
  SOURCES
    tests/test_geometry.cpp
)
