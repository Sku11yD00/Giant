giant_module(geometry
  SOURCES
    src/alembic.cpp
    src/clusters.cpp
    src/gltf.cpp
    src/groom.cpp
    src/pose_bake.cpp
    src/shapes.cpp
    src/vertex.cpp
  DEPENDS core anim
)
# Third-party code stays private: the geometry API is plain C++ types.
target_link_libraries(giant_geometry PRIVATE third_party::meshoptimizer third_party::cgltf)

giant_module_tests(geometry
  SOURCES
    tests/test_alembic.cpp
    tests/test_geometry.cpp
    tests/test_groom.cpp
    tests/test_pose_bake.cpp
)
