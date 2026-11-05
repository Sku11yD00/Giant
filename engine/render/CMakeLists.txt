giant_module(render
  SOURCES
    src/meshes.cpp
    src/renderer.cpp
  DEPENDS core rhi
)
giant_shaders(render
  SHADERS
    shaders/mesh.slang:vs_main:vertex:mesh_vs
    shaders/mesh.slang:ps_main:pixel:mesh_ps
)

giant_module_tests(render
  SOURCES
    tests/test_render.cpp
)
if(GIANT_BUILD_TESTS AND GIANT_REQUIRE_VALIDATION)
  target_compile_definitions(giant_render_tests PRIVATE GIANT_REQUIRE_VALIDATION=1)
endif()
