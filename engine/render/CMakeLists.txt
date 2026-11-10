giant_module(render
  SOURCES
    src/renderer.cpp
    src/decals.cpp
    src/streaming.cpp
    src/smaa.cpp
    src/voxel_scene.cpp
    src/instance_store.cpp
    src/terrain_paint.cpp
    src/shadow_pages.cpp
    src/voxel_light.cpp
    src/exposure.cpp
    src/brdf.cpp
  DEPENDS core rhi geometry voxel
)
giant_shaders(render
  SHADERS
    shaders/cull.slang:cs_reset:compute:cull_reset_cs
    shaders/cull.slang:cs_prepare:compute:cull_prepare_cs
    shaders/cull.slang:cs_instances:compute:cull_instances_cs
    shaders/cull.slang:cs_traverse:compute:cull_traverse_cs
    shaders/cull.slang:cs_retest:compute:cull_retest_cs
    shaders/skin.slang:cs_skin:compute:skin_cs
    shaders/hzb.slang:cs_first:compute:hzb_first_cs
    shaders/hzb.slang:cs_down:compute:hzb_down_cs
    shaders/raster.slang:ms_main:mesh:raster_ms
    shaders/raster.slang:vs_main:vertex:raster_vs
    shaders/raster.slang:ps_main:pixel:raster_ps
    shaders/raster.slang:ps_shadow:pixel:raster_shadow_ps
    shaders/shadow_clear.slang:cs_pages:compute:shadow_clear_pages_cs
    shaders/shadow_clear.slang:cs_words:compute:shadow_clear_words_cs
    shaders/resolve.slang:vs_main:vertex:resolve_vs
    shaders/resolve.slang:ps_main:pixel:resolve_ps
    shaders/resolve.slang:ps_direct:pixel:resolve_direct_ps
    shaders/reflect.slang:vs_main:vertex:reflect_vs
    shaders/reflect.slang:ps_main:pixel:reflect_ps
    shaders/smaa.slang:vs_main:vertex:smaa_vs
    shaders/smaa.slang:ps_edges:pixel:smaa_edges_ps
    shaders/smaa.slang:ps_weights:pixel:smaa_weights_ps
    shaders/smaa.slang:ps_blend:pixel:smaa_blend_ps
    shaders/voxels.slang:vs_main:vertex:voxels_vs
    shaders/voxels.slang:ps_main:pixel:voxels_ps
    shaders/voxel_light.slang:cs_clear:compute:light_clear_cs
    shaders/voxel_light.slang:cs_light:compute:light_voxels_cs
    shaders/sky.slang:cs_sky_view:compute:sky_view_cs
    shaders/sky.slang:cs_cloud_map:compute:cloud_map_cs
    shaders/sky.slang:cs_cloud_shadow:compute:cloud_shadow_cs
    shaders/sky.slang:cs_weather_cover:compute:weather_cover_cs
    shaders/sky.slang:cs_clouds:compute:clouds_cs
    shaders/sky.slang:cs_sky_multi:compute:sky_multi_cs
    shaders/sky.slang:cs_sky_aerial:compute:sky_aerial_cs
    shaders/sky.slang:cs_sky_ambient:compute:sky_ambient_cs
    shaders/ao.slang:cs_gtao:compute:ao_gtao_cs
    shaders/ao.slang:cs_denoise:compute:ao_denoise_cs
    shaders/ao.slang:cs_contact:compute:ao_contact_cs
    shaders/terrain.slang:cs_terrain:compute:terrain_cs
    shaders/post.slang:cs_histogram_clear:compute:histogram_clear_cs
    shaders/post.slang:cs_histogram:compute:histogram_cs
    shaders/post.slang:cs_bloom_down:compute:bloom_down_cs
    shaders/post.slang:cs_bloom_up:compute:bloom_up_cs
    shaders/post.slang:vs_main:vertex:post_vs
    shaders/post.slang:ps_main:pixel:post_ps
)
# SMAA's precomputed area and search tables (headers only).
target_link_libraries(giant_render PRIVATE third_party::smaa)

giant_module_tests(render
  SOURCES
    tests/test_render.cpp
    tests/test_decals.cpp
    tests/test_streaming.cpp
    tests/test_voxel_scene.cpp
    tests/test_shadow_pages.cpp
    tests/test_voxel_light.cpp
    tests/test_instance_store.cpp
    tests/test_skin.cpp
    tests/test_exposure.cpp
    tests/test_brdf.cpp
)
if(GIANT_BUILD_TESTS)
  # The streaming tests check the module's own streamer directly.
  target_include_directories(giant_render_tests PRIVATE src)
endif()
if(GIANT_BUILD_TESTS AND GIANT_REQUIRE_VALIDATION)
  target_compile_definitions(giant_render_tests PRIVATE GIANT_REQUIRE_VALIDATION=1)
endif()
