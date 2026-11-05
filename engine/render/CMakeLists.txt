giant_module(render
  SOURCES
    src/renderer.cpp
  DEPENDS core rhi geometry
)
giant_shaders(render
  SHADERS
    shaders/clusters.slang:vs_main:vertex:clusters_vs
    shaders/clusters.slang:ps_main:pixel:clusters_ps
)

giant_module_tests(render
  SOURCES
    tests/test_render.cpp
)
if(GIANT_BUILD_TESTS AND GIANT_REQUIRE_VALIDATION)
  target_compile_definitions(giant_render_tests PRIVATE GIANT_REQUIRE_VALIDATION=1)
endif()
