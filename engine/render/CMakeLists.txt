giant_module(render
  SOURCES
    src/renderer.cpp
    src/decals.cpp
    src/streaming.cpp
    src/smaa.cpp
  DEPENDS core rhi geometry
)
giant_shaders(render
  SHADERS
    shaders/cull.slang:cs_reset:compute:cull_reset_cs
    shaders/cull.slang:cs_prepare:compute:cull_prepare_cs
    shaders/cull.slang:cs_instances:compute:cull_instances_cs
    shaders/cull.slang:cs_traverse:compute:cull_traverse_cs
    shaders/cull.slang:cs_retest:compute:cull_retest_cs
    shaders/hzb.slang:cs_first:compute:hzb_first_cs
    shaders/hzb.slang:cs_down:compute:hzb_down_cs
    shaders/raster.slang:ms_main:mesh:raster_ms
    shaders/raster.slang:vs_main:vertex:raster_vs
    shaders/raster.slang:ps_main:pixel:raster_ps
    shaders/resolve.slang:vs_main:vertex:resolve_vs
    shaders/resolve.slang:ps_main:pixel:resolve_ps
    shaders/smaa.slang:vs_main:vertex:smaa_vs
    shaders/smaa.slang:ps_edges:pixel:smaa_edges_ps
    shaders/smaa.slang:ps_weights:pixel:smaa_weights_ps
    shaders/smaa.slang:ps_blend:pixel:smaa_blend_ps
)
# SMAA's precomputed area and search tables (headers only).
target_link_libraries(giant_render PRIVATE third_party::smaa)

giant_module_tests(render
  SOURCES
    tests/test_render.cpp
    tests/test_decals.cpp
    tests/test_streaming.cpp
)
if(GIANT_BUILD_TESTS)
  # The streaming tests check the module's own streamer directly.
  target_include_directories(giant_render_tests PRIVATE src)
endif()
if(GIANT_BUILD_TESTS AND GIANT_REQUIRE_VALIDATION)
  target_compile_definitions(giant_render_tests PRIVATE GIANT_REQUIRE_VALIDATION=1)
endif()
