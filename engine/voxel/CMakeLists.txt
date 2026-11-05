giant_module(voxel
  SOURCES
    src/brick.cpp
    src/clipmap.cpp
  DEPENDS core
)

giant_module_tests(voxel
  SOURCES
    tests/test_voxel.cpp
)
