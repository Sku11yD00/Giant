# Shared settings for every engine module.

function(giant_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
      -Wnon-virtual-dtor -Woverloaded-virtual)
  endif()
endfunction()

function(giant_apply_sanitizers target)
  if(GIANT_SANITIZE AND GIANT_SANITIZE_THREAD)
    message(FATAL_ERROR "GIANT_SANITIZE and GIANT_SANITIZE_THREAD cannot be combined")
  endif()
  if(GIANT_SANITIZE_THREAD AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
  if(GIANT_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# giant_module(<name> SOURCES ... [DEPENDS ...])
# Creates the library giant_<name> from engine/<name>, with public headers in
# engine/<name>/include. Allowed DEPENDS are listed in docs/architecture.md.
function(giant_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(target giant_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  add_library(giant::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} PUBLIC giant::${dep})
  endforeach()
  giant_apply_warnings(${target})
  giant_apply_sanitizers(${target})
endfunction()

# giant_d3d12_app(<executable>)
# On Windows: the executable uses the DirectX 12 Agility SDK runtime, copied
# into a D3D12 folder next to it, and the pinned WARP software GPU.
function(giant_d3d12_app target)
  if(NOT WIN32)
    return()
  endif()
  target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/d3d12_agility_exports.cpp)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/cmake/d3d12_agility_exports.cpp
    TARGET_DIRECTORY ${target}
    PROPERTIES COMPILE_DEFINITIONS GIANT_D3D12_SDK_VERSION=${GIANT_D3D12_SDK_VERSION})
  set(agility ${GIANT_D3D12_AGILITY_DIR}/build/native/bin/x64)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/D3D12
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${agility}/D3D12Core.dll ${agility}/d3d12SDKLayers.dll
            $<TARGET_FILE_DIR:${target}>/D3D12
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${GIANT_D3D12_WARP_DIR}/build/native/bin/x64/d3d10warp.dll $<TARGET_FILE_DIR:${target}>
    VERBATIM)
endfunction()

# giant_module_tests(<name> SOURCES ...)
# Builds one test executable per module and registers it with CTest.
function(giant_module_tests name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT GIANT_BUILD_TESTS)
    return()
  endif()
  set(target giant_${name}_tests)
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE giant::${name} giant::test_main)
  giant_apply_warnings(${target})
  giant_apply_sanitizers(${target})
  giant_d3d12_app(${target}) # any test may open a GPU device
  add_test(NAME ${name} COMMAND ${target})
  # A hung test fails instead of stalling CI for hours.
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()
