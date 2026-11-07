set(GIANT_RHI_SOURCES
  src/rhi.cpp
  src/null/device_null.cpp
)
if(WIN32)
  list(APPEND GIANT_RHI_SOURCES src/d3d12/device_d3d12.cpp)
endif()

giant_module(rhi
  SOURCES ${GIANT_RHI_SOURCES}
  DEPENDS core
)
giant_shaders(rhi
  SHADERS
    shaders/ui.slang:vs_main:vertex:ui_vs
    shaders/ui.slang:ps_main:pixel:ui_ps
)

if(WIN32)
  target_compile_definitions(giant_rhi PUBLIC GIANT_RHI_HAS_D3D12=1)
  # Agility SDK headers first, so d3d12.h is the version we ship.
  target_link_libraries(giant_rhi PRIVATE third_party::d3d12_headers d3d12 dxgi dxguid)
else()
  target_compile_definitions(giant_rhi PUBLIC GIANT_RHI_HAS_D3D12=0)
endif()

giant_module_tests(rhi
  SOURCES
    tests/test_rhi.cpp
)
if(GIANT_BUILD_TESTS AND GIANT_REQUIRE_VALIDATION)
  target_compile_definitions(giant_rhi_tests PRIVATE GIANT_REQUIRE_VALIDATION=1)
endif()
if(GIANT_BUILD_TESTS)
  # The heap ranges are private bookkeeping, tested directly.
  target_include_directories(giant_rhi_tests PRIVATE src)
endif()
if(GIANT_BUILD_TESTS)
  giant_shaders(rhi_test TARGET giant_rhi_tests
    SHADERS
      tests/shaders/test_compute.slang:cs_fill:compute:test_fill_cs
      tests/shaders/test_compute.slang:cs_ramp:compute:test_ramp_cs
      tests/shaders/test_draw.slang:vs_main:vertex:test_draw_vs
      tests/shaders/test_draw.slang:ps_main:pixel:test_draw_ps
      tests/shaders/test_draw.slang:ps_write:pixel:test_draw_write_ps
      tests/shaders/test_mesh.slang:ms_main:mesh:test_mesh_ms
      tests/shaders/test_mesh.slang:ps_main:pixel:test_mesh_ps
  )
endif()
