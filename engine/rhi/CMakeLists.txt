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

if(WIN32)
  target_compile_definitions(giant_rhi PUBLIC GIANT_RHI_HAS_D3D12=1)
  target_link_libraries(giant_rhi PRIVATE d3d12 dxgi dxguid)
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
