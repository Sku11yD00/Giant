giant_module(script
  SOURCES
    src/script_host.cpp
  DEPENDS core
)
if(NOT WIN32)
  target_link_libraries(giant_script PRIVATE ${CMAKE_DL_LIBS})
endif()

# The C# side: the engine's API (Giant.Scripting), the sample scripts and the
# scripts these tests load, built with the .NET SDK into build/managed.
if(GIANT_DOTNET)
  set(managed ${CMAKE_BINARY_DIR}/managed)
  set(dotnet_build ${GIANT_DOTNET} build --nologo -v quiet -c Release
      --artifacts-path ${CMAKE_BINARY_DIR}/managed-obj)
  add_custom_target(giant_managed ALL
    COMMAND ${dotnet_build} -o ${managed}
            ${PROJECT_SOURCE_DIR}/scripts/Giant.Scripting/Giant.Scripting.csproj
    COMMAND ${dotnet_build} -o ${managed}/samples/HelloGiant
            ${PROJECT_SOURCE_DIR}/scripts/samples/HelloGiant/HelloGiant.csproj
    COMMAND ${dotnet_build} -o ${managed}/tests/ScriptTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/managed/ScriptTests/ScriptTests.csproj
    COMMENT "Building the C# API and scripts"
    VERBATIM
  )
endif()

giant_module_tests(script
  SOURCES
    tests/test_script_host.cpp
)
if(GIANT_BUILD_TESTS)
  if(GIANT_DOTNET)
    add_dependencies(giant_script_tests giant_managed)
    target_compile_definitions(giant_script_tests PRIVATE
      GIANT_TEST_MANAGED_DIR="${CMAKE_BINARY_DIR}/managed")
  endif()
  if(GIANT_SANITIZE_THREAD)
    target_compile_definitions(giant_script_tests PRIVATE GIANT_SKIP_DOTNET_RUNTIME_TESTS=1)
  endif()
  # .NET handles its own segfaults (null references) and its GC confuses the
  # leak checker; ThreadSanitizer cannot follow .NET's threads at all, so the
  # runtime tests are skipped there (see the test file).
  set_tests_properties(script PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=handle_segv=0:allow_user_segv_handler=1:detect_leaks=0:use_sigaltstack=0")
endif()
